/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */
/**
 * @file exec_instruction_gc.cpp
 * @brief Implementacion de las instrucciones de gestion de memoria del GC de VestaVM.
 *
 * Implementa las instrucciones ALLOC, FREE, REALLOC, NEWOBJ, DROP, GCRUN,
 * GCWB, GCCONFIG, GCALLOC, GCDEREF y las operaciones de cursor (READCUR, WRITECUR).
 */#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"
#include "loader/oop_types.h"
#include "util/simd_copy.h"

namespace runtime {

    // -------------------------------------------------------------------------
    // Cursor - acceso a memoria real del host - opcodes 0x00 0xC0 .. 0xC2
    // Los registros cursor (CUR0-CUR3) contienen punteros host directos.
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta la instruccion READCUR: lee de la memoria host apuntada por un cursor.
     *
     * Obtiene la direccion host almacenada en el registro cursor indicado y lee
     * 1, 2, 4 u 8 bytes segun el modo codificado.  El resultado se almacena en
     * el registro general destino extendido a 64 bits.
     *
     * @param vm    Proceso virtual que ejecuta READCUR.
     * @param instr Instruccion descodificada con reg_data.reg2 (cursor) y reg_data.reg1 (destino).
     */
    void exec_instr_readcur(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx = instr.data_instruction.reg_data.reg2;    // indice del cursor (0-3)
        const uint8_t  dst     = instr.data_instruction.reg_data.reg1;    // registro general destino
        const uint64_t addr    = vm->registers.cur[cur_idx].qword();      // puntero host del cursor

        // leer segun el modo: 8, 16, 32 o 64 bits
        switch (instr.flags_info.mode) {
            case 0b00: vm->registers.regs[dst].qword(*reinterpret_cast<const uint8_t  *>(addr)); break; //  8 bits
            case 0b01: vm->registers.regs[dst].qword(*reinterpret_cast<const uint16_t *>(addr)); break; // 16 bits
            case 0b10: vm->registers.regs[dst].qword(*reinterpret_cast<const uint32_t *>(addr)); break; // 32 bits
            default:   vm->registers.regs[dst].qword(*reinterpret_cast<const uint64_t *>(addr)); break; // 64 bits
        }
    }

    /**
     * @brief Ejecuta la instruccion WRITECUR: escribe en la memoria host apuntada por un cursor.
     *
     * Obtiene la direccion host del cursor indicado y escribe el valor del registro
     * fuente truncado al tamano indicado por el modo (1, 2, 4 u 8 bytes).
     *
     * @param vm    Proceso virtual que ejecuta WRITECUR.
     * @param instr Instruccion descodificada con reg_data.reg2 (cursor) y reg_data.reg1 (fuente).
     */
    void exec_instr_writecur(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx = instr.data_instruction.reg_data.reg2;  // indice del cursor (0-3)
        const uint8_t  src     = instr.data_instruction.reg_data.reg1;  // registro general fuente
        const uint64_t addr    = vm->registers.cur[cur_idx].qword();    // puntero host del cursor
        const uint64_t val     = vm->registers.regs[src].qword();       // valor a escribir

        // escribir segun el modo: 8, 16, 32 o 64 bits
        switch (instr.flags_info.mode) {
            case 0b00: *reinterpret_cast<uint8_t  *>(addr) = static_cast<uint8_t >(val); break; //  8 bits
            case 0b01: *reinterpret_cast<uint16_t *>(addr) = static_cast<uint16_t>(val); break; // 16 bits
            case 0b10: *reinterpret_cast<uint32_t *>(addr) = static_cast<uint32_t>(val); break; // 32 bits
            default:   *reinterpret_cast<uint64_t *>(addr) = val;                        break; // 64 bits
        }
    }

    /**
     * @brief Ejecuta la instruccion GCDEREF: desreferencia un handle GC en un cursor.
     *
     * Convierte el handle GC almacenado en el registro indicado en un puntero
     * host al payload del objeto y lo escribe en el registro cursor destino.
     * El cursor queda listo para accesos directos con READCUR/WRITECUR.
     *
     * @param vm    Proceso virtual que ejecuta GCDEREF.
     * @param instr Instruccion descodificada con reg_data.reg2 (cursor) y reg_data.reg1 (handle).
     */
    /**
     * @brief Ejecuta ADDCUR: suma un inmediato con signo al registro cursor curN.
     *
     * Lee el inmediato de 16 bits almacenado en los bytes 2-3 de raw_data.raw1
     * (colocado alli por decode_instr_addcur) y lo suma con extension de signo
     * al valor de 64 bits del cursor.  Permite avanzar (imm > 0) y retroceder
     * (imm < 0) sin el patron xchg/adds/xchg de tres instrucciones.
     *
     * @param vm    Proceso virtual que ejecuta ADDCUR.
     * @param instr Instruccion descodificada: reg2=cur_idx, raw1[2..3]=imm16.
     */
    void exec_instr_addcur(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t cur_idx = instr.data_instruction.reg_data.reg2 & 0x3; // cursor 0-3

        // recuperar imm16 de los bytes 2-3 de raw_data.raw1 (ver decode_instr_addcur)
        const auto *raw = reinterpret_cast<const uint8_t *>(&instr.data_instruction.raw_data.raw1);
        const int16_t imm16 = static_cast<int16_t>(
            static_cast<uint16_t>(raw[2]) | (static_cast<uint16_t>(raw[3]) << 8)
        );

        // sumar con extension de signo a 64 bits para que el retroceso funcione
        const uint64_t old_val = vm->registers.cur[cur_idx].qword();
        vm->registers.cur[cur_idx].qword(
            static_cast<uint64_t>(static_cast<int64_t>(old_val) + imm16)
        );
    }

    /**
     * @brief Ejecuta VMCOPY: copia bytes desde VM memory a host memory (cursor).
     *
     * Lee rLen bytes desde VM memory a partir de la direccion virtual almacenada
     * en el registro rSrc y los escribe en la memoria host apuntada por curN.
     * Usa la funcion simd_copy::fast_copy para elegir en tiempo de ejecucion la
     * ruta SIMD mas rapida (AVX-512, AVX2, SSE2 o memcpy escalar).
     *
     * Tras la copia avanza automaticamente:
     *   - curN  += rLen  (cursor host apunta al byte siguiente al ultimo copiado)
     *   - rSrc  += rLen  (puntero VM avanza para copias secuenciales)
     *
     * @param vm    Proceso virtual que ejecuta VMCOPY.
     * @param instr Instruccion descodificada: mem_data.reg_final=cur_idx,
     *              mem_data.reg_base=rSrc, mem_data.reg_index=rLen.
     */
    void exec_instr_vmcopy(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx  = instr.data_instruction.mem_data.reg_final & 0x3; // cursor 0-3
        const uint8_t  r_src    = instr.data_instruction.mem_data.reg_base;          // reg VM src
        const uint8_t  r_len    = instr.data_instruction.mem_data.reg_index;         // reg longitud

        const uint64_t vm_addr  = vm->registers.regs[r_src].qword();                // dir. virtual
        const uint64_t len      = vm->registers.regs[r_len].qword();                // bytes a copiar
        uint8_t *const dst      = reinterpret_cast<uint8_t *>(vm->registers.cur[cur_idx].qword());

        if (len == 0) return; // copia vacia: no modificar nada

        // leer bytes desde VM memory al buffer host apuntado por el cursor;
        // read_bytes maneja cruces de pagina y lazy allocation internamente
        // se usa un buffer temporal solo si dst no es valido, pero aqui dst es host ptr
        vm->vm_mem.read_bytes(vm_addr, dst, static_cast<size_t>(len));

        // avanzar cursor host y puntero VM para facilitar copias secuenciales
        vm->registers.cur[cur_idx].qword(vm->registers.cur[cur_idx].qword() + len);
        vm->registers.regs[r_src].qword(vm_addr + len);
    }

    /**
     * @brief Ejecuta VCOPYH: copia bytes desde host memory (cursor) a VM memory.
     *
     * Inverso de VMCOPY: lee rLen bytes del buffer host apuntado por curN y los
     * escribe en VM memory a partir de la direccion virtual almacenada en rDst.
     * Avanza curN y rDst en rLen bytes tras la copia.
     *
     * @param vm    Proceso virtual que ejecuta VCOPYH.
     * @param instr Instruccion descodificada: mem_data.reg_final=cur_idx,
     *              mem_data.reg_base=rDst, mem_data.reg_index=rLen.
     */
    void exec_instr_vcopyh(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx  = instr.data_instruction.mem_data.reg_final & 0x3; // cursor 0-3
        const uint8_t  r_dst    = instr.data_instruction.mem_data.reg_base;          // reg VM dst
        const uint8_t  r_len    = instr.data_instruction.mem_data.reg_index;         // reg longitud

        const uint64_t vm_addr  = vm->registers.regs[r_dst].qword();                // dir. virtual
        const uint64_t len      = vm->registers.regs[r_len].qword();                // bytes a copiar
        const uint8_t *src      = reinterpret_cast<const uint8_t *>(vm->registers.cur[cur_idx].qword());

        if (len == 0) return;

        // escribir desde host memory al espacio virtual de la VM
        vm->vm_mem.write_bytes(vm_addr, src, static_cast<size_t>(len));

        // avanzar cursor host y puntero VM
        vm->registers.cur[cur_idx].qword(vm->registers.cur[cur_idx].qword() + len);
        vm->registers.regs[r_dst].qword(vm_addr + len);
    }

    // -------------------------------------------------------------------------
    // Consulta de entorno de ejecucion - opcodes 0x00 0xC6..0xC8
    // Exponen punteros del entorno como uint64_t para pasarlos a funciones
    // nativas via calln sin modificar la convencion de llamada existente.
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta GETPROC: carga el puntero al ProcessVM actual en el registro destino.
     */
    void exec_instr_getproc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t rdst = instr.data_instruction.reg_data.reg1; // registro destino
        vm->registers.regs[rdst].qword(reinterpret_cast<uint64_t>(vm));
    }

    /**
     * @brief Ejecuta GETVM: carga el puntero a la instancia VM propietaria en el registro destino.
     */
    void exec_instr_getvm(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t rdst = instr.data_instruction.reg_data.reg1;
        // sigue la cadena: proceso -> scheduler -> vm_reference (VM&)
        vm->registers.regs[rdst].qword(reinterpret_cast<uint64_t>(&vm->scheduler.vm_reference));
    }

    /**
     * @brief Ejecuta GETMGR: carga el puntero al ManageVM del gestor en el registro destino.
     */
    void exec_instr_getmgr(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t rdst = instr.data_instruction.reg_data.reg1;
        // sigue la cadena: proceso -> scheduler -> vm_reference -> mgr_vm (ManageVM&)
        vm->registers.regs[rdst].qword(reinterpret_cast<uint64_t>(&vm->scheduler.vm_reference.mgr_vm));
    }

    void exec_instr_gcderef(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx    = instr.data_instruction.reg_data.reg2;          // cursor destino (0-3)
        const uint8_t  handle_reg = instr.data_instruction.reg_data.reg1;          // registro con el handle GC
        const uint64_t raw_handle = vm->registers.regs[handle_reg].qword();       // valor del handle

        // desreferenciar el handle para obtener el puntero host al payload
        uint8_t *payload = vm->gc_heap.deref(static_cast<gc::GcHandle>(raw_handle));
        vm->registers.cur[cur_idx].qword(reinterpret_cast<uint64_t>(payload));    // guardar puntero en cursor
    }

    /**
     * @brief Devuelve el PID encoded del proceso actual en r_dst.
     *
     * Formato: (scheduler_id << 32) | (local_pid & 0xFFFFFFFF).  Mismo
     * encoding que SPAWN deposita en R0 al crear un hijo.  Util para
     * que el proceso pueda enviarse a si mismo via mailbox o registrar
     * su PID en estructuras compartidas.
     */
    void exec_instr_getpid(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_dst = instr.data_instruction.reg_data.reg1;
        const uint64_t encoded =
            (static_cast<uint64_t>(vm->pid.scheduler_id) << 32) |
             static_cast<uint64_t>(vm->pid.local_pid & 0xFFFFFFFFu);
        vm->registers.regs[r_dst].qword(encoded);
    }

    /**
     * @brief Inverso de GCDEREF: dado un puntero host al payload, devuelve el GcHandle.
     *
     * Lee el host_ptr del registro reg2 (operando 2: source) y consulta el mapa
     * inverso del GcHeap.  Escribe el resultado (GcHandle uint32, o GC_NULL_HANDLE
     * si el puntero no corresponde a un objeto vivo) en reg1 (operando 1: dest).
     */
    void exec_instr_gchandle(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_dst   = instr.data_instruction.reg_data.reg1;            // dest reg
        const uint8_t  r_src   = instr.data_instruction.reg_data.reg2;            // src reg con host_ptr
        const auto     ptr_val = static_cast<uintptr_t>(vm->registers.regs[r_src].qword());
        const auto    *payload = reinterpret_cast<const uint8_t *>(ptr_val);
        gc::GcHandle   h       = vm->gc_heap.handle_for_ptr(payload);
        vm->registers.regs[r_dst].qword(static_cast<uint64_t>(h));                // GC_NULL_HANDLE si no encontrado
    }

    // -------------------------------------------------------------------------
    // GC generacional - opcodes 0x00 0xA0 .. 0xA5
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta la instruccion NEWOBJ: aloca un objeto gestionado por el GC.
     *
     * Lee el puntero a ClassInfo del registro indicado y aloca instance_size bytes
     * en el heap GC.  Si la aloca tiene exito, inicializa el ObjectHeader con el
     * puntero a la clase, el flag GC_OWNED y el handle como hash inicial.
     * El handle resultante (o GC_NULL_HANDLE en caso de fallo) se escribe en R00.
     *
     * @param vm    Proceso virtual que ejecuta NEWOBJ.
     * @param instr Instruccion descodificada con reg_data.reg1 (registro con ClassInfo*).
     */
    void exec_instr_newobj(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1; // registro con el puntero ClassInfo
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (cls == nullptr) {
            // clase nula: devolver handle invalido
            vm->registers.regs[R00].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
            return;
        }

        gc::GcHandle h = vm->gc_heap.alloc(static_cast<size_t>(cls->instance_size)); // alocar en el heap GC

        if (h != gc::GC_NULL_HANDLE) {
            uint8_t *payload = vm->gc_heap.deref(h); // obtener puntero host al payload
            if (payload != nullptr) {
                auto *hdr      = reinterpret_cast<loader::ObjectHeader *>(payload);
                hdr->class_ptr = cls;                                     // enlazar la clase
                hdr->flags     = loader::OBJ_FLAG_GC_OWNED;               // marcar como gestionado por GC
                hdr->hash_code = static_cast<uint32_t>(h);                // usar el handle como hash inicial
            }
        }

        vm->registers.regs[R00].qword(static_cast<uint64_t>(h)); // devolver el handle en R00
    }

    /**
     * @brief Ejecuta la instruccion GCRUN: lanza un ciclo de GC menor.
     *
     * Fuerza la recoleccion de basura de la generacion joven.  Puede llamarse
     * desde bytecode para controlar la presion de memoria en puntos conocidos.
     *
     * @param vm    Proceso virtual que ejecuta GCRUN.
     * @param instr Instruccion descodificada (no se usan sus campos).
     */
    void exec_instr_gcrun(ProcessVM *vm, const DecodedInstr &instr) {
        (void)instr;         // instruccion sin operandos
        vm->gc_heap.minor_gc(); // ejecutar el GC menor
    }

    /**
     * @brief Ejecuta la instruccion GCCONFIG: configura el umbral de promocion del GC.
     *
     * Lee el valor del umbral desde el registro fuente segun el modo y llama a
     * set_old_threshold() para ajustar el limite de bytes tras el cual los objetos
     * se promueven a la generacion antigua.
     *
     * @param vm    Proceso virtual que ejecuta GCCONFIG.
     * @param instr Instruccion descodificada con reg_data.reg1 (fuente) y mode.
     */
    void exec_instr_gcconfig(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc      = instr.data_instruction.reg_data.reg1; // registro con el nuevo umbral
        const uint64_t threshold = read_reg_table[instr.flags_info.mode](vm, rsrc); // leer segun modo
        vm->gc_heap.set_old_threshold(static_cast<size_t>(threshold)); // aplicar la configuracion
    }

    /**
     * @brief Ejecuta la instruccion GCDROP: libera un objeto del heap GC por handle.
     *
     * Decrementa el contador de referencias o libera el objeto segun la politica
     * del GC.  El handle se lee del registro fuente segun el modo.
     *
     * @param vm    Proceso virtual que ejecuta GCDROP.
     * @param instr Instruccion descodificada con reg_data.reg1 (fuente) y mode.
     */
    void exec_instr_gc_drop(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc   = instr.data_instruction.reg_data.reg1; // registro con el handle a liberar
        const uint64_t handle = read_reg_table[instr.flags_info.mode](vm, rsrc);
        vm->gc_heap.drop(static_cast<gc::GcHandle>(handle)); // liberar el handle en el GC
    }

    /**
     * @brief Ejecuta la instruccion GCWB: notifica al GC una escritura (write barrier).
     *
     * Debe llamarse antes de sobrescribir un puntero GC para que el GC pueda
     * mantener la consistencia de sus estructuras de rastreo generacional.
     *
     * @param vm    Proceso virtual que ejecuta GCWB.
     * @param instr Instruccion descodificada con reg_data.reg1 (fuente) y mode.
     */
    void exec_instr_gcwb(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc       = instr.data_instruction.reg_data.reg1; // registro con el handle antiguo
        const uint64_t old_handle = read_reg_table[instr.flags_info.mode](vm, rsrc);
        vm->gc_heap.write_barrier(static_cast<gc::GcHandle>(old_handle)); // notificar la escritura al GC
    }

    /**
     * @brief Ejecuta la instruccion GCALLOC: aloca un bloque de bytes en el heap GC.
     *
     * A diferencia de NEWOBJ, no inicializa ninguna cabecera de objeto.  Util para
     * alocar buffers o arreglos gestionados por el GC.  El handle resultante se
     * devuelve en R00.
     *
     * @param vm    Proceso virtual que ejecuta GCALLOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (tamano) y mode.
     */
    void exec_instr_gcalloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1; // registro con el tamano en bytes
        const uint64_t size = read_reg_table[instr.flags_info.mode](vm, rsrc);

        gc::GcHandle h = vm->gc_heap.alloc(static_cast<size_t>(size)); // alocar en el heap GC
        vm->registers.regs[R00].qword(static_cast<uint64_t>(h));       // devolver el handle en R00
    }

    // -------------------------------------------------------------------------
    // Raw allocator - opcodes 0x00 0xB0 .. 0xB2
    // Memoria no gestionada por el GC; ciclo de vida manual.
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta la instruccion RAWALLOC: aloca memoria bruta no gestionada por el GC.
     *
     * El tamano se obtiene del registro fuente segun el modo.  El puntero host
     * al bloque alocalizado se escribe en R00 (0 si falla).
     *
     * @param vm    Proceso virtual que ejecuta RAWALLOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (tamano) y mode.
     */
    void exec_instr_raw_alloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1; // registro con el tamano
        const uint64_t size = read_reg_table[instr.flags_info.mode](vm, rsrc);

        uint64_t ptr = vm->raw_alloc.alloc(static_cast<size_t>(size)); // alocar bloque
        vm->registers.regs[R00].qword(ptr); // devolver puntero host en R00
    }

    /**
     * @brief Ejecuta la instruccion RAWFREE: libera un bloque de memoria bruta.
     *
     * El registro fuente contiene el puntero host al bloque previamente alocalizado
     * con RAWALLOC o RAWREALLOC.
     *
     * @param vm    Proceso virtual que ejecuta RAWFREE.
     * @param instr Instruccion descodificada con reg_data.reg1 (puntero a liberar).
     */
    void exec_instr_raw_free(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1; // registro con el puntero a liberar
        const uint64_t ptr  = vm->registers.regs[rsrc].qword();
        vm->raw_alloc.free(ptr); // liberar el bloque
    }

    /**
     * @brief Ejecuta la instruccion RAWREALLOC: redimensiona un bloque de memoria bruta.
     *
     * Lee el puntero actual del primer registro y el nuevo tamano del segundo.
     * El puntero al bloque redimensionado (que puede diferir del original) se
     * devuelve en R00.
     *
     * @param vm    Proceso virtual que ejecuta RAWREALLOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (ptr) y reg_data.reg2 (size).
     */
    void exec_instr_raw_realloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rptr  = instr.data_instruction.reg_data.reg1; // registro con el puntero original
        const uint8_t  rsize = instr.data_instruction.reg_data.reg2; // registro con el nuevo tamano
        const uint64_t ptr   = vm->registers.regs[rptr].qword();     // puntero al bloque original
        const uint64_t size  = vm->registers.regs[rsize].qword();    // nuevo tamano en bytes

        uint64_t new_ptr = vm->raw_alloc.realloc(ptr, static_cast<size_t>(size)); // redimensionar
        vm->registers.regs[R00].qword(new_ptr); // devolver el nuevo puntero en R00
    }

} // namespace runtime
