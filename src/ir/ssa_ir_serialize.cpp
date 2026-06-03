/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ssa_ir_serialize.cpp
 * @brief Serializer/deserializer binario del IR SSA para embeber en .velb.
 *
 * El IR generado por el frontend Vex se puede persistir dentro del
 * binario @c .velb en una seccion @c @ir (magic @c VEIR).  Esto permite:
 *
 *   - **JIT a partir del binario sin recompilar**: el loader recupera el
 *     IR del .velb y lo pasa al JitCompiler.  Sin esto, el JIT solo
 *     podria trabajar con codigo compilado in-process durante una
 *     ejecucion (perdiendo el speedup post-warmup en runs cortos).
 *
 *   - **AOT futuro (Phase D.10+)**: el optimizer del JIT puede operar
 *     ANTES de la ejecucion sobre el IR del .velb y persistir codigo
 *     nativo en un .velao adicional.
 *
 *   - **Debugging / tooling**: dumps del IR sirven para ver lo que el
 *     frontend genero sin tener que recompilar con flags especiales.
 *
 * Diseno del formato:
 *   - **Little-endian** consistente con el resto del .velb.
 *   - **Schema fijo por @c IrInstr**: serializamos SIEMPRE los mismos
 *     campos (op, type, dst, flags, source_line, imm, operands,
 *     func_name, func_ptr, target_block, false_block, phi_args)
 *     incluso si una op concreta no usa alguno.  Trade-off: ~10-20% mas
 *     de espacio que un schema variable, pero parser/writer mucho mas
 *     simples y robustos.  Una v2 podria optimizar via "op -> field set"
 *     (variable-length encoding por op) cuando el espacio importe.
 *   - **Strings con length prefix u32**: simple, no requiere escapes ni
 *     terminadores; tolera bytes arbitrarios (incluyendo NUL embebidos).
 *   - **IDs como u32**: cabe cualquier IR razonable (4G values/blocks)
 *     sin gastar el espacio de un u64.
 */

#include "ir/ssa_ir_serialize.h"

#include <cstring>

namespace ir {

    /* ===================================================================== */
    /* IrValue                                                                */
    /* ===================================================================== */

    namespace {

        // Bits del byte flags por IrValue.  Compactamos 5 bools en un byte
        // en lugar de un byte por flag (ahorra ~5 bytes por SSA value).
        // El orden es estable -- NO reordenar entre versiones del formato
        // sin bump del version field, porque los .velb antiguos quedarian
        // ininterpretables.
        constexpr uint8_t IRVAL_FLAG_PARAM            = 1 << 0; ///< Parametro formal de la funcion
        constexpr uint8_t IRVAL_FLAG_CONST            = 1 << 1; ///< Constante compile-time (const_val valido)
        constexpr uint8_t IRVAL_FLAG_HOST_PTR         = 1 << 2; ///< Puntero a memoria host (no VM)
        constexpr uint8_t IRVAL_FLAG_POINTEE_HOST_PTR = 1 << 3; ///< Puntero VM cuyo contenido apunta a host
        constexpr uint8_t IRVAL_FLAG_GC_OBJECT        = 1 << 4; ///< Host_ptr a objeto GC-managed

        // Bits del byte flags por IrInstr.  Solo 2 bits usados: el resto
        // queda para extensiones futuras sin cambio de formato.
        constexpr uint8_t INSTR_FLAG_PRESERVE      = 1 << 0; ///< No eliminar aunque dst este muerto (side-effect implicito)
        constexpr uint8_t INSTR_FLAG_IS_CALL_SITE  = 1 << 1; ///< Marca instruccion como call site (para stackmaps)
        constexpr uint8_t INSTR_FLAG_HOST_ALLOCA   = 1 << 2; ///< ALLOCA auto-promovida a host stack (Phase D.jit-mem-model)
        constexpr uint8_t INSTR_FLAG_HOST_ALLOCA_EXPLICIT_FREE = 1 << 3; ///< Sprint mem-loop-fix: RAW_FREE preservado para liberar in-loop

        // Bits del byte flags por IrFunction.
        constexpr uint8_t FN_FLAG_NATIVE   = 1 << 0; ///< Funcion FFI (sin body IR; solo declaracion)
        constexpr uint8_t FN_FLAG_VARIADIC = 1 << 1; ///< Acepta nargs variable (R15 contiene el count)

        /**
         * @brief Serializa un @c IrValue al stream binario.
         *
         * Layout: @c [u8 type][u8 flags][u64 const_val if is_const].
         * El @c id NO se serializa porque se recupera del indice en
         * @c IrFunction::values[].  Total: 2 bytes para values normales,
         * 10 bytes para constantes (el @c const_val solo se emite si
         * @c is_const, ahorrando 8 bytes por value no-constante; en una
         * funcion tipica >70% de los values no son constantes, asi que
         * el ahorro acumulado es significativo).
         */
        void write_value(std::vector<uint8_t> &o, const IrValue &v) {
            write_u8(o, static_cast<uint8_t>(v.type));
            // Compactar los 5 bools en un byte para minimizar el footprint
            // del IR serializado (los values son la mayoria del .velb IR).
            uint8_t flags = 0;
            if (v.is_param)            flags |= IRVAL_FLAG_PARAM;
            if (v.is_const)            flags |= IRVAL_FLAG_CONST;
            if (v.is_host_ptr)         flags |= IRVAL_FLAG_HOST_PTR;
            if (v.pointee_is_host_ptr) flags |= IRVAL_FLAG_POINTEE_HOST_PTR;
            if (v.is_gc_object)        flags |= IRVAL_FLAG_GC_OBJECT;
            write_u8(o, flags);
            // Optimizacion: const_val solo ocupa espacio si el value es
            // realmente una constante.  Una funcion tipica tiene ~30%
            // constantes, el resto son SSA values normales que no necesitan
            // const_val.  El deserializer respeta el flag.
            if (v.is_const) {
                write_u64(o, v.const_val);
            }
        }

        /**
         * @brief Deserializa un @c IrValue desde el stream binario.
         *
         * Inverso exacto de @c write_value.  Los @c read_u8 / @c read_u64
         * devuelven false si no hay suficientes bytes restantes; cualquier
         * fallo propaga inmediatamente.  Si el bit @c is_const no esta
         * activo, @c const_val queda en 0 (valor default del IrValue
         * recien construido).
         */
        bool read_value(const std::vector<uint8_t> &in, size_t &off, IrValue &v) {
            uint8_t type_byte = 0, flags = 0;
            // Lecturas defensivas: si el buffer se acaba, abort temprano.
            if (!read_u8(in, off, type_byte)) return false;
            if (!read_u8(in, off, flags))     return false;
            v.type      = static_cast<IrType>(type_byte);
            // Decodificacion explicita de cada bit a su bool correspondiente.
            // El compilador idealmente lo colapsa a operaciones bitwise + cmovs.
            v.is_param            = (flags & IRVAL_FLAG_PARAM)            != 0;
            v.is_const            = (flags & IRVAL_FLAG_CONST)            != 0;
            v.is_host_ptr         = (flags & IRVAL_FLAG_HOST_PTR)         != 0;
            v.pointee_is_host_ptr = (flags & IRVAL_FLAG_POINTEE_HOST_PTR) != 0;
            v.is_gc_object        = (flags & IRVAL_FLAG_GC_OBJECT)        != 0;
            // const_val solo se leyo si el writer la emitio; sin el flag
            // queda en su valor default (0).  Mismo principio que write_value.
            if (v.is_const) {
                if (!read_u64(in, off, v.const_val)) return false;
            }
            return true;
        }

        /**
         * @brief Serializa un @c IrInstr completo al stream binario.
         *
         * Tamano por instr: ~30 bytes minimo + 4 por operando + tamano
         * del func_name + 8 por phi_arg.  En IR tipico (~200 instrs por
         * funcion) -> ~10-15 KB por funcion serializada.
         *
         * Schema fijo: emitimos TODOS los campos aunque la op especifica
         * no los use.  Ejemplo: @c IrOp::ADD no usa @c target_block ni
         * @c phi_args pero igual los emitimos como 0 / vacio.  Esto
         * sacrifica espacio por simplificar el formato y permite que el
         * deserializer use un solo path sin tener que conocer el set de
         * campos por op.
         */
        void write_instr(std::vector<uint8_t> &o, const IrInstr &i) {
            // op va en u16 porque el set de opcodes ya pasa los 200 (no
            // cabe en u8) y queremos espacio para crecer sin cambio de
            // formato.
            write_u16(o, static_cast<uint16_t>(i.op));
            write_u8(o,  static_cast<uint8_t>(i.type));
            // dst es IrValueId (u32 en el storage).  IR_NO_VALUE (UINT32_MAX)
            // indica "esta op no produce valor" (e.g. STORE, RET void).
            write_u32(o, static_cast<uint32_t>(i.dst));
            uint8_t flags = 0;
            if (i.preserve)     flags |= INSTR_FLAG_PRESERVE;
            if (i.is_call_site) flags |= INSTR_FLAG_IS_CALL_SITE;
            if (i.host_alloca)  flags |= INSTR_FLAG_HOST_ALLOCA;
            if (i.host_alloca_explicit_free) flags |= INSTR_FLAG_HOST_ALLOCA_EXPLICIT_FREE;
            write_u8(o, flags);
            // source_line: util para diagnosticos y stack traces.  0 si
            // el frontend no aporto info de linea.
            write_u32(o, i.source_line);
            // imm: campo polivalente.  Para CONST contiene el valor; para
            // CALL contiene flags/args adicionales; para ops sin imm es 0.
            write_u64(o, i.imm);
            // operands: lista de IrValueId.  Maximo 255 por op (suficiente
            // para cualquier op concebible; CALL con mas args es raro pero
            // tampoco esperamos > 255 args reales en codigo Vex normal).
            const size_t opc = i.operands.size();
            write_u8(o, opc > 255 ? 255 : static_cast<uint8_t>(opc));
            for (size_t k = 0; k < opc && k < 255; ++k) {
                write_u32(o, static_cast<uint32_t>(i.operands[k]));
            }
            // func_name: solo significativo para CALL/CALLN/CALLIND.
            // Para otras ops queda como cadena vacia (write_str emite
            // un u32=0 length sin payload).
            write_str(o, i.func_name);
            // func_ptr (puntero a funcion para CALLIND/CALLCLOSURE) +
            // target_block y false_block (para BR/BR_COND).  Cero si la
            // op no los usa.
            write_u32(o, static_cast<uint32_t>(i.func_ptr));
            write_u32(o, static_cast<uint32_t>(i.target_block));
            write_u32(o, static_cast<uint32_t>(i.false_block));
            // phi_args: lista de (block, value) para PHI nodes.  Vacio
            // para ops no-PHI.  Mismo limite de 255 por simetria con
            // operands.
            const size_t pc = i.phi_args.size();
            write_u8(o, pc > 255 ? 255 : static_cast<uint8_t>(pc));
            for (size_t k = 0; k < pc && k < 255; ++k) {
                write_u32(o, static_cast<uint32_t>(i.phi_args[k].value));
                write_u32(o, static_cast<uint32_t>(i.phi_args[k].block));
            }
        }

        /**
         * @brief Deserializa un @c IrInstr completo desde el stream.
         *
         * Inverso exacto de @c write_instr.  Cualquier @c read_* que
         * falle (buffer truncado) propaga inmediatamente con false.
         *
         * Garantia post-condicion: si retorna true, todos los campos de
         * @c i estan poblados con valores consistentes.  Los vectors
         * (@c operands, @c phi_args) fueron clear+reserve+push para
         * evitar fragmentacion del heap incluso en programas con muchas
         * funciones.
         */
        bool read_instr(const std::vector<uint8_t> &in, size_t &off, IrInstr &i) {
            uint16_t op_v = 0;
            uint8_t  type_v = 0, flags = 0;
            uint32_t dst_v = 0, source_line = 0;
            uint64_t imm = 0;
            // Lecturas en exactamente el mismo orden que el writer; los
            // fallos cortan toda la operacion (NO intentamos recovery
            // parcial: una IR malformada no es util incluso si recuperamos
            // la primera mitad).
            if (!read_u16(in, off, op_v))         return false;
            if (!read_u8 (in, off, type_v))       return false;
            if (!read_u32(in, off, dst_v))        return false;
            if (!read_u8 (in, off, flags))        return false;
            if (!read_u32(in, off, source_line))  return false;
            if (!read_u64(in, off, imm))          return false;
            i.op            = static_cast<IrOp>(op_v);
            i.type          = static_cast<IrType>(type_v);
            i.dst           = static_cast<IrValueId>(dst_v);
            i.preserve      = (flags & INSTR_FLAG_PRESERVE)      != 0;
            i.is_call_site  = (flags & INSTR_FLAG_IS_CALL_SITE)  != 0;
            i.host_alloca   = (flags & INSTR_FLAG_HOST_ALLOCA)   != 0;
            i.host_alloca_explicit_free = (flags & INSTR_FLAG_HOST_ALLOCA_EXPLICIT_FREE) != 0;
            i.source_line   = source_line;
            i.imm           = imm;
            /* operands */
            uint8_t opc = 0;
            if (!read_u8(in, off, opc)) return false;
            i.operands.clear();
            i.operands.reserve(opc);
            for (uint8_t k = 0; k < opc; ++k) {
                uint32_t v = 0;
                if (!read_u32(in, off, v)) return false;
                i.operands.push_back(static_cast<IrValueId>(v));
            }
            /* func_name */
            if (!read_str(in, off, i.func_name)) return false;
            /* func_ptr / target_block / false_block */
            uint32_t fp = 0, tb = 0, fb = 0;
            if (!read_u32(in, off, fp)) return false;
            if (!read_u32(in, off, tb)) return false;
            if (!read_u32(in, off, fb)) return false;
            i.func_ptr     = static_cast<IrValueId>(fp);
            i.target_block = static_cast<IrBlockId>(tb);
            i.false_block  = static_cast<IrBlockId>(fb);
            /* phi_args */
            uint8_t pc = 0;
            if (!read_u8(in, off, pc)) return false;
            i.phi_args.clear();
            i.phi_args.reserve(pc);
            for (uint8_t k = 0; k < pc; ++k) {
                uint32_t v = 0, b = 0;
                if (!read_u32(in, off, v)) return false;
                if (!read_u32(in, off, b)) return false;
                IrPhiArg a{static_cast<IrValueId>(v),
                           static_cast<IrBlockId>(b)};
                i.phi_args.push_back(a);
            }
            return true;
        }

        void write_block(std::vector<uint8_t> &o, const IrBlock &b) {
            write_str(o, b.name);
            write_u32(o, static_cast<uint32_t>(b.instrs.size()));
            for (const auto &i : b.instrs) write_instr(o, i);
            write_u32(o, static_cast<uint32_t>(b.preds.size()));
            for (auto p : b.preds) write_u32(o, static_cast<uint32_t>(p));
            write_u32(o, static_cast<uint32_t>(b.succs.size()));
            for (auto s : b.succs) write_u32(o, static_cast<uint32_t>(s));
        }

        bool read_block(const std::vector<uint8_t> &in, size_t &off, IrBlock &b) {
            if (!read_str(in, off, b.name)) return false;
            uint32_t n_instrs = 0;
            if (!read_u32(in, off, n_instrs)) return false;
            b.instrs.clear();
            b.instrs.reserve(n_instrs);
            for (uint32_t k = 0; k < n_instrs; ++k) {
                IrInstr i;
                if (!read_instr(in, off, i)) return false;
                b.instrs.push_back(std::move(i));
            }
            uint32_t n_preds = 0;
            if (!read_u32(in, off, n_preds)) return false;
            b.preds.clear();
            b.preds.reserve(n_preds);
            for (uint32_t k = 0; k < n_preds; ++k) {
                uint32_t v = 0;
                if (!read_u32(in, off, v)) return false;
                b.preds.push_back(static_cast<IrBlockId>(v));
            }
            uint32_t n_succs = 0;
            if (!read_u32(in, off, n_succs)) return false;
            b.succs.clear();
            b.succs.reserve(n_succs);
            for (uint32_t k = 0; k < n_succs; ++k) {
                uint32_t v = 0;
                if (!read_u32(in, off, v)) return false;
                b.succs.push_back(static_cast<IrBlockId>(v));
            }
            return true;
        }

    } // namespace anonymous

    /* ===================================================================== */
    /* serialize_function / deserialize_function                              */
    /* ===================================================================== */

    /**
     * @brief Serializa una @c IrFunction completa.
     *
     * @return Numero de bytes que se escribieron en @c out (la posicion
     *         final menos la inicial).  Util si el caller quiere saber
     *         el tamano de esta funcion concreta sin recalcular.
     *
     * Orden de campos en el stream:
     *   1. name (string)
     *   2. ret_type (u8) + fn_flags (u8: native, variadic)
     *   3. params: count u32 + IrValueId[count]
     *   4. values: count u32 + IrValue[count]  (cada uno con su layout)
     *   5. blocks: count u32 + IrBlock[count]
     *   6. metadata de generics: template_name + n type_args + type_args[]
     */
    size_t serialize_function(const IrFunction &fn,
                              std::vector<uint8_t> &out) {
        const size_t start = out.size();

        write_str(out, fn.name);
        write_u8(out, static_cast<uint8_t>(fn.ret_type));
        // Empaquetar los 2 bools de la funcion en un solo byte (mismo
        // patron que en values).
        uint8_t fn_flags = 0;
        if (fn.is_native)   fn_flags |= FN_FLAG_NATIVE;
        if (fn.is_variadic) fn_flags |= FN_FLAG_VARIADIC;
        write_u8(out, fn_flags);

        // Params: lista de IrValueId que apuntan a entries en values[]
        // que tienen is_param=true.  Duplicacion intencional para que el
        // lookup "i-esimo parametro" sea O(1) sin filtrar values[].
        write_u32(out, static_cast<uint32_t>(fn.params.size()));
        for (auto p : fn.params) write_u32(out, static_cast<uint32_t>(p));

        // Values: TODOS los SSA values de la funcion.  El indice en este
        // array ES el IrValueId (los ids son densos 0..N-1).
        write_u32(out, static_cast<uint32_t>(fn.values.size()));
        for (const auto &v : fn.values) write_value(out, v);

        // Blocks: cada uno incluye sus propias instrs + preds + succs.
        // El indice en este array ES el IrBlockId.
        write_u32(out, static_cast<uint32_t>(fn.blocks.size()));
        for (const auto &b : fn.blocks) write_block(out, b);

        // Metadata para generics monomorphizados: el frontend Vex setea
        // template_name (e.g. "List") y type_args (e.g. ["i32"]) al
        // monomorphizar @c List<i32>.  Util para tools que quieran
        // mostrar el template original en stack traces; el JIT no lo usa.
        write_str(out, fn.generic_template_name);
        write_u32(out, static_cast<uint32_t>(fn.generic_type_args.size()));
        for (const auto &s : fn.generic_type_args) write_str(out, s);

        return out.size() - start;
    }

    bool deserialize_function(const std::vector<uint8_t> &in,
                              size_t &off,
                              IrFunction &out) {
        out = IrFunction{};

        if (!read_str(in, off, out.name)) return false;
        uint8_t ret_type_b = 0, fn_flags = 0;
        if (!read_u8(in, off, ret_type_b)) return false;
        if (!read_u8(in, off, fn_flags))   return false;
        out.ret_type    = static_cast<IrType>(ret_type_b);
        out.is_native   = (fn_flags & FN_FLAG_NATIVE)   != 0;
        out.is_variadic = (fn_flags & FN_FLAG_VARIADIC) != 0;

        /* params */
        uint32_t n_params = 0;
        if (!read_u32(in, off, n_params)) return false;
        out.params.clear();
        out.params.reserve(n_params);
        for (uint32_t k = 0; k < n_params; ++k) {
            uint32_t v = 0;
            if (!read_u32(in, off, v)) return false;
            out.params.push_back(static_cast<IrValueId>(v));
        }

        /* values */
        uint32_t n_values = 0;
        if (!read_u32(in, off, n_values)) return false;
        out.values.clear();
        out.values.reserve(n_values);
        for (uint32_t k = 0; k < n_values; ++k) {
            IrValue v;
            v.id = static_cast<IrValueId>(k);  /* id es el indice en el vector */
            if (!read_value(in, off, v)) return false;
            out.values.push_back(std::move(v));
        }

        /* blocks */
        uint32_t n_blocks = 0;
        if (!read_u32(in, off, n_blocks)) return false;
        out.blocks.clear();
        out.blocks.reserve(n_blocks);
        for (uint32_t k = 0; k < n_blocks; ++k) {
            IrBlock b;
            b.id = static_cast<IrBlockId>(k);  /* id es el indice */
            if (!read_block(in, off, b)) return false;
            out.blocks.push_back(std::move(b));
        }

        /* metadata */
        if (!read_str(in, off, out.generic_template_name)) return false;
        uint32_t n_args = 0;
        if (!read_u32(in, off, n_args)) return false;
        out.generic_type_args.clear();
        out.generic_type_args.reserve(n_args);
        for (uint32_t k = 0; k < n_args; ++k) {
            std::string s;
            if (!read_str(in, off, s)) return false;
            out.generic_type_args.push_back(std::move(s));
        }
        return true;
    }

    /* ===================================================================== */
    /* emit_ir_section / parse_ir_section                                     */
    /* ===================================================================== */

    /**
     * @brief Emite la seccion @c @ir completa para el .velb.
     *
     * Layout de la seccion:
     *   - Header (12 bytes):
     *     - u32 magic (@c VEIR)
     *     - u16 version
     *     - u16 reserved (0 para alineamiento, futuras flags)
     *     - u32 fn_count
     *   - fn_count funciones concatenadas (cada una serializada por
     *     @c serialize_function).
     *
     * El caller (linker .velb) toma el resultado y lo escribe en una
     * seccion dedicada del binario.  El loader, al detectar la seccion,
     * llama a @c parse_ir_section para reconstruir el vector de IrFunctions.
     */
    std::vector<uint8_t> emit_ir_section(const std::vector<IrFunction> &functions) {
        std::vector<uint8_t> out;
        // Pre-reserva: header (~16 B) + funcion tipica (~512 B).  Un
        // sobreestimador modesto evita realocaciones del vector durante
        // la serializacion sin desperdiciar memoria.
        out.reserve(64 + functions.size() * 512);

        // Header: identifica la seccion + version para validacion del
        // loader y permite future-proofing si cambiamos el schema.
        write_u32(out, IR_SECTION_MAGIC);
        write_u16(out, IR_SECTION_VERSION);
        write_u16(out, 0);  // reserved (siempre 0 en v1; lo verifica el reader)
        write_u32(out, static_cast<uint32_t>(functions.size()));

        // Funciones concatenadas back-to-back.  No hay padding entre
        // ellas: el parser usa el size de cada funcion (implicito en
        // los campos) para avanzar al siguiente.
        for (const auto &fn : functions) {
            (void)serialize_function(fn, out);  // ignoramos el retorno aqui
        }

        return out;
    }

    /**
     * @brief Parsea la seccion @c @ir desde un buffer (tipicamente el
     *        mmap del .velb).
     *
     * @param data         Buffer completo del .velb (lectura solo).
     * @param offset       Offset dentro de @c data donde empieza @c VEIR.
     * @param section_size Tamano de la seccion (segun el header del .velb).
     * @param functions    Vector que rellenamos.  Se @c clear primero, asi
     *                     llamar repetidamente sobre el mismo vector funciona.
     * @return @c true si el parseo fue exitoso; @c false en cualquier error
     *         (magic incorrecto, version no soportada, buffer truncado,
     *         fn_count sospechosamente grande).
     *
     * Politica de seguridad: este parser puede leer bytes que vienen de
     * un .velb potencialmente corrupto o malicioso.  Por eso validamos:
     *   - Magic correcto (VEIR).
     *   - Version exacta (rechazamos versiones futuras desconocidas; un
     *     v2 podria tener layout incompatible).
     *   - fn_count <= 100000 (hard cap defensivo).
     *   - Cada deserializacion individual no excede @c section_end.
     */
    bool parse_ir_section(const std::vector<uint8_t> &data,
                          size_t offset,
                          size_t section_size,
                          std::vector<IrFunction> &functions) {
        functions.clear();

        // Validar el tamano minimo (al menos el header completo de 12 bytes:
        // magic 4 + version 2 + reserved 2 + fn_count 4) y que la seccion
        // no se salga del buffer.
        if (section_size < 12) return false;
        if (offset + section_size > data.size()) return false;

        size_t off = offset;
        uint32_t magic = 0;
        if (!read_u32(data, off, magic)) return false;
        // Magic check: si no es VEIR, el caller paso un offset
        // equivocado o la seccion esta corrupta.  Abortar antes de
        // intentar parsear nada interpretable.
        if (magic != IR_SECTION_MAGIC) return false;

        uint16_t version = 0;
        uint16_t reserved = 0;
        if (!read_u16(data, off, version)) return false;
        if (!read_u16(data, off, reserved)) return false;
        // Version exact match: si fuera un .velb v2 con un schema
        // distinto, parsearlo como v1 daria garbage.  Mejor rechazar
        // explicitamente y forzar al usuario a usar el loader correcto.
        if (version != IR_SECTION_VERSION) return false;

        uint32_t fn_count = 0;
        if (!read_u32(data, off, fn_count)) return false;

        // Hard cap defensivo: programas con >100000 funciones IR son
        // imposibles en la practica (incluso un programa enorme tendria
        // ~5000 funciones).  Sin este cap, un .velb malicioso con un
        // fn_count enorme (UINT32_MAX) forzaria a @c functions.reserve a
        // intentar alocar TBs de memoria, crasheando con OOM.
        if (fn_count > 100000) return false;

        functions.reserve(fn_count);
        const size_t section_end = offset + section_size;
        // Loop de deserializacion: cada funcion avanza @c off por su
        // propio tamano.  Verificamos antes (off >= end -> truncada) y
        // despues (off > end -> overflow de la seccion).
        for (uint32_t i = 0; i < fn_count; ++i) {
            // Si ya pasamos el fin de la seccion antes de empezar a leer
            // esta funcion, el fn_count es inconsistente con section_size.
            if (off >= section_end) return false;
            IrFunction fn;
            if (!deserialize_function(data, off, fn)) return false;
            // Validacion post-deserialize: la funcion no debe haberse
            // "comido" mas bytes de los que pertenecen a la seccion.
            if (off > section_end) return false;
            functions.push_back(std::move(fn));
        }
        return true;
    }

} // namespace ir
