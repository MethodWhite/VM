/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file ssa_ir.h
 * @brief Representacion Intermedia SSA (Static Single Assignment) para VestaVM.
 *
 * Este header define las estructuras de datos de la IR intermedia que un
 * compilador de lenguaje de alto nivel debe producir antes de bajar a
 * bytecode .vel de VestaVM.
 *
 * La IR usa la forma SSA: cada variable se asigna exactamente una vez.
 * Los puntos de union de flujo de control usan instrucciones Phi para
 * seleccionar el valor correcto segun el bloque predecesor.
 *
 * Pipeline tipico de un compilador HLL sobre VestaVM:
 *
 *   Fuente HLL -> AST -> Analisis semantico -> SSA IR -> Optimizador -> Emisor .vel
 *
 * La SSA IR de VestaVM es intencional y deliberadamente simple:
 *   - Sin tipos de registro (todos los valores son IrValue de 64 bits).
 *   - Tipos de alto nivel se anotan con IrType para el emisor y la reflexion.
 *   - No hay calculo de liveness ni coloreado de registros en la IR:
 *     el emisor asigna registros VM de forma greedy con pool de 16 (r0-r15).
 *
 * Formato de texto (archivo .ir):  ver doc/VMdoc/IR/SSA.md
 *
 * Ejemplo minimo:
 * @code
 *   @function add(a: i64, b: i64) -> i64 {
 *   entry:
 *       %0 = add.i64 %a, %b
 *       ret.i64 %0
 *   }
 * @endcode
 */

#ifndef SSA_IR_H
#define SSA_IR_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// Windows (windef.h) define VOID->void, CONST->const, BOOL->int como macros,
// lo que rompe los enumerados de la IR.  Guardamos y anulamos antes de entrar.
#ifdef _WIN32
#  pragma push_macro("VOID")
#  pragma push_macro("CONST")
#  pragma push_macro("BOOL")
#  undef VOID
#  undef CONST
#  undef BOOL
#endif

namespace ir {

    // =========================================================================
    //  Tipos de la IR
    // =========================================================================

    /**
     * @brief Tipo de un valor en la SSA IR de VestaVM.
     *
     * Todos los valores se representan como 64 bits en los registros VM.
     * El tipo anotado sirve para:
     *   - Elegir la instruccion correcta al bajar a bytecode (add vs. fadd).
     *   - Generar metadatos de reflexion en ClassInfo/FieldInfo.
     *   - Emitir advertencias de tipo en el optimizador.
     */
    enum class IrType : uint8_t {
        VOID   = 0,  ///< sin valor (tipo de retorno void)
        I8     = 1,  ///< entero de 8 bits con signo
        I16    = 2,  ///< entero de 16 bits con signo
        I32    = 3,  ///< entero de 32 bits con signo
        I64    = 4,  ///< entero de 64 bits con signo (tipo por defecto)
        U8     = 5,  ///< entero de 8 bits sin signo
        U16    = 6,  ///< entero de 16 bits sin signo
        U32    = 7,  ///< entero de 32 bits sin signo
        U64    = 8,  ///< entero de 64 bits sin signo
        F32    = 9,  ///< float  IEEE 754 de 32 bits
        F64    = 10, ///< double IEEE 754 de 64 bits (bits almacenados como u64)
        PTR    = 11, ///< puntero host (uint64_t cast)
        HANDLE = 12, ///< GcHandle de 32 bits (almacenado en los 32 bits bajos)
        BOOL   = 13, ///< booleano (0 = false, != 0 = true)
    };

    /**
     * @brief Convierte un IrType a su nombre en el formato de texto.
     * @param t Tipo a convertir.
     * @return Cadena del tipo ("i64", "f64", "handle", ...).
     */
    const char *ir_type_name(IrType t);

    /**
     * @brief Parsea el nombre de un tipo del formato de texto.
     * @param name Nombre del tipo ("i64", "u32", ...).
     * @param out  Tipo parseado.
     * @return true si el nombre es valido.
     */
    bool ir_type_parse(const char *name, IrType &out);

    // =========================================================================
    //  Codigos de operacion de la IR
    // =========================================================================

    /**
     * @brief Codigos de operacion de las instrucciones SSA IR.
     *
     * Nomenclatura: el sufijo del texto (.i64, .f64, .u32) es el IrType
     * del operando/resultado, no del opcode.  El opcode solo indica la
     * operacion; el tipo se anota en IrInstr::type.
     *
     * Grupos:
     *   0x00..0x0F  Constantes y movimiento
     *   0x10..0x1F  Aritmetica entera
     *   0x20..0x2F  Aritmetica flotante
     *   0x30..0x3F  Logica y desplazamientos
     *   0x40..0x4F  Comparaciones
     *   0x50..0x5F  Conversiones de tipo
     *   0x60..0x6F  Flujo de control
     *   0x70..0x7F  SSA / phi
     *   0x80..0x8F  Llamadas (VM, nativas, virtuales)
     *   0x90..0x9F  Memoria
     *   0xA0..0xAF  OOP / GC
     *   0xB0..0xBF  Manejo de excepciones
     *   0xC0..0xCF  Async / futures
     *   0xD0..0xDF  Distribucion (mensajes, spawn remoto)
     *   0xE0..0xEF  Sincronizacion / monitores
     *   0xF0..0xFF  Intrinsics VM (proceso, scheduler, etc.)
     */
    enum class IrOp : uint16_t {
        // ---- constantes y movimiento (0x00-0x0F) ----
        CONST        = 0x00, ///< %dst = const.T  imm64
        MOV          = 0x01, ///< %dst = mov.T   %src   (copia; eliminada en lowering)
        NOP          = 0x02, ///< nop
        STR_LIT_ADDR = 0x03, ///< %dst = str_lit_addr.ptr  imm=indice en IrModule::static_data
                              ///<   El emisor genera "mov rDst, @Absolute(\"code.s_<imm>\")"
                              ///<   resolviendo a la direccion VM del literal en la seccion data
                              ///<   adjuntada al final de la seccion "code".  Tipo destino: PTR.

        // ---- aritmetica entera (0x10-0x1F) ----
        ADD      = 0x10, ///< %dst = add.T    %a, %b
        SUB      = 0x11, ///< %dst = sub.T    %a, %b
        MUL      = 0x12, ///< %dst = mul.T    %a, %b
        DIV      = 0x13, ///< %dst = div.T    %a, %b  (con signo segun T)
        MOD      = 0x14, ///< %dst = mod.T    %a, %b
        NEG      = 0x15, ///< %dst = neg.T    %a       (negacion entera unaria)

        // ---- aritmetica flotante (0x20-0x2F) ----
        FADD     = 0x20, ///< %dst = fadd.fN  %a, %b
        FSUB     = 0x21, ///< %dst = fsub.fN  %a, %b
        FMUL     = 0x22, ///< %dst = fmul.fN  %a, %b
        FDIV     = 0x23, ///< %dst = fdiv.fN  %a, %b
        FNEG     = 0x24, ///< %dst = fneg.fN  %a        (negacion flotante unaria)
        FABS     = 0x25, ///< %dst = fabs.fN  %a        (valor absoluto)
        FSQRT    = 0x26, ///< %dst = fsqrt.fN %a        (raiz cuadrada)
        FMIN     = 0x27, ///< %dst = fmin.fN  %a, %b
        FMAX     = 0x28, ///< %dst = fmax.fN  %a, %b
        // 0x29..0x2B reservados (antes FFLOOR/FCEIL/FROUND, eliminados; el
        // frontend Vex baja a CALLN(stdlib/native/math/vesta_math:vmath_*)).

        // ---- logica y desplazamientos (0x30-0x3F) ----
        AND      = 0x30, ///< %dst = and.T    %a, %b
        OR       = 0x31, ///< %dst = or.T     %a, %b
        XOR      = 0x32, ///< %dst = xor.T    %a, %b
        NOT      = 0x33, ///< %dst = not.T    %a
        SHL      = 0x34, ///< %dst = shl.T    %a, %n   (desplazamiento a izquierda)
        SHR      = 0x35, ///< %dst = shr.T    %a, %n   (logico, sin signo)
        SAR      = 0x36, ///< %dst = sar.T    %a, %n   (aritmetico, con signo)

        // ---- comparaciones enteras (0x40-0x47) ----
        CMP_EQ   = 0x40, ///< %dst = cmp.eq.T  %a, %b  -> bool
        CMP_NE   = 0x41, ///< %dst = cmp.ne.T  %a, %b  -> bool
        CMP_LT   = 0x42, ///< %dst = cmp.lt.T  %a, %b  -> bool (con signo)
        CMP_GT   = 0x43, ///< %dst = cmp.gt.T  %a, %b  -> bool
        CMP_LE   = 0x44, ///< %dst = cmp.le.T  %a, %b  -> bool
        CMP_GE   = 0x45, ///< %dst = cmp.ge.T  %a, %b  -> bool
        CMP_ULT  = 0x46, ///< %dst = cmp.ult.T %a, %b  -> bool (sin signo)
        CMP_UGT  = 0x47, ///< %dst = cmp.ugt.T %a, %b  -> bool
        CMP_ULE  = 0x48, ///< %dst = cmp.ule.T %a, %b  -> bool
        CMP_UGE  = 0x49, ///< %dst = cmp.uge.T %a, %b  -> bool

        // ---- comparaciones flotantes (0x4A-0x4F) ----
        FCMP_EQ  = 0x4A, ///< %dst = fcmp.eq.fN %a, %b -> bool  (ordered)
        FCMP_NE  = 0x4B, ///< %dst = fcmp.ne.fN %a, %b -> bool
        FCMP_LT  = 0x4C, ///< %dst = fcmp.lt.fN %a, %b -> bool
        FCMP_GT  = 0x4D, ///< %dst = fcmp.gt.fN %a, %b -> bool
        FCMP_LE  = 0x4E, ///< %dst = fcmp.le.fN %a, %b -> bool
        FCMP_GE  = 0x4F, ///< %dst = fcmp.ge.fN %a, %b -> bool

        // ---- conversiones de tipo (0x50-0x5F) ----
        CAST     = 0x50, ///< %dst = cast.T    %src  (truncar/extender/reinterpretar)
        ZEXT     = 0x51, ///< %dst = zext.T    %src  (zero-extend a tipo mayor)
        SEXT     = 0x52, ///< %dst = sext.T    %src  (sign-extend a tipo mayor)
        TRUNC    = 0x53, ///< %dst = trunc.T   %src  (truncar a tipo menor)
        ITOF     = 0x54, ///< %dst = itof.fN   %src  (entero con signo a flotante)
        UITOF    = 0x55, ///< %dst = uitof.fN  %src  (entero sin signo a flotante)
        FTOI     = 0x56, ///< %dst = ftoi.T    %src  (flotante a entero con signo)
        FTOUI    = 0x57, ///< %dst = ftoui.T   %src  (flotante a entero sin signo)
        F32TOF64 = 0x58, ///< %dst = f32tof64  %src  (widening: f32 -> f64)
        F64TOF32 = 0x59, ///< %dst = f64tof32  %src  (narrowing: f64 -> f32)
        BITCAST  = 0x5A, ///< %dst = bitcast.T %src  (reinterpretar bits sin conversion)

        // ---- flujo de control (0x60-0x6F) ----
        BR       = 0x60, ///< br  label                    (salto incondicional)
        BR_COND  = 0x61, ///< br.cond %cond, L_true, L_false
        RET      = 0x62, ///< ret.T %val  /  ret.void
        UNREACHABLE = 0x63, ///< unreachable               (codigo inalcanzable)

        // ---- SSA (0x70-0x7F) ----
        PHI      = 0x70, ///< %dst = phi.T [%v0, L0], [%v1, L1], ...

        // ---- llamadas (0x80-0x8F) ----
        CALL     = 0x80, ///< %dst = call.T    @fn(%a, %b, ...)        (intra-modulo)
        CALLIND  = 0x81, ///< %dst = callind.T %fn_ptr(%a, ...)        (puntero de funcion)
        TAILCALL = 0x82, ///< tailcall @fn(%a, %b, ...)                (tail-call intra)
        CALLVIRT = 0x83, ///< %dst = callvirt.T %obj, vtbl_idx(%a, ...) (virtual via vtable)
        CALLN    = 0x84, ///< %dst = calln.T   @lib:func(%a, ...)      (nativa FFI calln)
        CALLM    = 0x85, ///< %dst = callm.T   %obj, %method(%a, ...)  (dispatch via MethodInfo*; A.5.2.b interfaces, reflexion)
        CALLCLOSURE = 0x86, ///< %dst = callclosure.T %fn_ptr, %env(%a, ...)  (Phase A.10:
                            ///< llamada a closure inline.  Identico a CALLIND pero ademas
                            ///< coloca @c env en R14 antes del @c callvm fn_ptr.  Si la
                            ///< lambda no captura nada, env = 0 (sentinela).  El campo
                            ///< @c func_ptr lleva el SSA del fn_addr; el primer operando
                            ///< es el env_ptr; los restantes son los args declarados.
                            ///< Lowering en exec: @c lower_lambda_expr emite el helper
                            ///< sintetico __lambda_<N> y el call site usa este opcode.)

        // ---- memoria (0x90-0x9F) ----
        ALLOCA   = 0x90, ///< %dst = alloca.T count       (reservar en pila local)
        LOAD     = 0x91, ///< %dst = load.T  %ptr         (leer; movh si is_host_ptr, mov si no)
        STORE    = 0x92, ///< store.T  %val, %ptr         (escribir; idem LOAD)
        MEMCPY   = 0x93, ///< memcpy %dst_ptr, %src_ptr, %len
        RAW_ALLOC = 0x94, ///< %dst = raw_alloc.ptr %size  (rawalloc; dst es puntero host)
        RAW_FREE  = 0x95, ///< raw_free %ptr               (rawfree)

        // ---- OOP / GC (0xA0-0xAF) ----
        NEWOBJ      = 0xA0, ///< %dst = newobj  %class_ptr          (allojar objeto GC)
        GETFIELD    = 0xA1, ///< %dst = getfield.T  %obj, field_off (gcderef+addcur+readcur)
        SETFIELD    = 0xA2, ///< setfield.T %obj, field_off, %val   (writecur + gcwb si HANDLE)
        INSTANCEOF  = 0xA3, ///< %dst = instanceof %obj, %class_ptr -> bool
        CHECKCAST   = 0xA4, ///< checkcast %obj, %class_ptr         (throw si falla)
        ISNULL      = 0xA5, ///< %dst = isnull %src                 (r = (src==0)?1:0)
        UNWRAP      = 0xA6, ///< %dst = unwrap %src                 (throw NullPointer si 0)
        SPECIALIZE  = 0xA7, ///< %dst = specialize %class, %types, count
        GEP         = 0xA8, ///< %ptr = gep.ptr %handle, imm_off    (byte-offset en objeto GC via cursor)
        GCWB_IR     = 0xA9, ///< gcwb_ir %handle                    (write barrier explicito al GC)
        ARRAY_ALLOC = 0xAA, ///< %h = array_alloc.T %len            (allojar array tipado; helper nativo)
        ARRAY_LEN   = 0xAB, ///< %n = array_len.i64 %arr            (leer campo length del array)
        ARRAY_LOAD  = 0xAC, ///< %v = array_load.T %arr, %idx       (carga con MOVC SIB stride)
        ARRAY_STORE = 0xAD, ///< array_store.T %arr, %idx, %val     (escritura + gcwb si HANDLE)
        GCDEREF_IR  = 0xAE, ///< gcderef_ir %handle                 (gcderef a cur0; ver nota)

        // ---- manejo de excepciones (0xB0-0xBF) ----
        THROW       = 0xB0, ///< throw %exc_obj                     (lanzar excepcion)
        TRYENTER    = 0xB1, ///< tryenter %handler_pc, %class_ptr   (push ExceptionFrame)
        TRYLEAVE    = 0xB2, ///< tryleave                           (pop ExceptionFrame)
        LANDINGPAD  = 0xB3, ///< %exc = landingpad.T                (receptor del objeto en catch)
        // -- operaciones de cadena (0xB4-0xBF): bajan a instrucciones VM strmake..strfinalize --
        STRMAKE     = 0xB4, ///< %h = strmake.handle %buf, %len [enc=imm]
        STRLEN      = 0xB5, ///< %n = strlen.i64 %str_handle
        STRCAT      = 0xB6, ///< %h = strcat.handle %a, %b
        STRCMP      = 0xB7, ///< %r = strcmp.i64 %a, %b   (-1/0/1)
        STRSLICE    = 0xB8, ///< %h = strslice.handle %str, %range
        STRFLAT     = 0xB9, ///< %h = strflat.handle %str
        STRHASH     = 0xBA, ///< %n = strhash.u64 %str
        STRINTERN   = 0xBB, ///< %h = strintern.handle %str
        STRRAW      = 0xBC, ///< %p = strraw.ptr %str              (host pointer para FFI)
        STRCONV     = 0xBD, ///< %h = strconv.handle %str, enc=imm
        STRRESERVE  = 0xBE, ///< %h = strreserve.handle %cap       (FLAT con capacidad)
        STRFINALIZE = 0xBF, ///< strfinalize %str, %new_len        (actualizar byte_len+hash)

        // ---- async / futures (0xC0-0xCF) ----
        FUTURE    = 0xC0, ///< %dst = future                    (crear FutureObject PENDING)
        AWAIT     = 0xC1, ///< %dst = await.T %future_handle    (bloquear hasta resolver)
        FULFILL   = 0xC2, ///< fulfill  %future_handle, %value  (resolver con valor)
        REJECT    = 0xC3, ///< reject   %future_handle, %error  (rechazar con codigo)

        // ---- distribucion (0xD0-0xDF) ----
        MSGSEND   = 0xD0, ///< %dst = msgsend %pid, %buf_addr, %len -> bool (1=ok)
        MSGRECV   = 0xD1, ///< %dst = msgrecv.T %max_len, %buf_addr -> bytes  (bloquea)
        RSPAWN    = 0xD2, ///< %dst = rspawn   %node_idx, %fn_addr -> future_handle

        // ---- sincronizacion / monitores (0xE0-0xEF) ----
        MONENTER  = 0xE0, ///< monenter %obj     (adquirir monitor reentrable)
        MONEXIT   = 0xE1, ///< monexit  %obj     (liberar monitor)
        MONWAIT   = 0xE2, ///< monwait  %obj     (liberar y esperar; re-adquirir al despertar)
        MONNOTI   = 0xE3, ///< monnoti  %obj     (despertar un esperante)
        MONNOTA   = 0xE4, ///< monnota  %obj     (despertar todos los esperantes)

        // ---- intrinsics VM (0xF0-0xFF) ----
        GETPROC   = 0xF0, ///< %dst = getproc     (ProcessVM* del proceso actual)
        GETVM     = 0xF1, ///< %dst = getvm       (VM* de la instancia)
        GETMGR    = 0xF2, ///< %dst = getmgr      (ManageVM* del gestor global)
        SPAWN     = 0xF3, ///< %dst = spawn    %fn_ptr     (crear proceso hijo)
        RESUME    = 0xF4, ///< resume          %pid         (despertar proceso)
        YIELD     = 0xF5, ///< yield                        (ceder quantum al scheduler)
        SWAPCTX   = 0xF6, ///< swapctx %dst_ctx, %src_ctx  (cambio de contexto cooperativo)

        // ---- codigo ensamblador incrustado (0xFF) ----
        RAW_ASM   = 0xFF, ///< raw_asm "texto"  (ensamblador .vel verbatim; nunca optimizado)
    };

    /**
     * @brief Convierte un IrOp a su nombre en el formato de texto.
     * @param op Operacion a convertir.
     * @return Nombre de texto del opcode.
     */
    const char *ir_op_name(IrOp op);

    /**
     * @brief Parsea el nombre de un opcode del formato de texto.
     * @param name Nombre del opcode (p.ej. "add", "calln", "monenter").
     * @param out  Opcode parseado.
     * @return true si el nombre es valido.
     */
    bool ir_op_parse(const char *name, IrOp &out);

    // =========================================================================
    //  Valores SSA
    // =========================================================================

    /**
     * @brief Identificador unico de un valor SSA.
     *
     * Un IrValueId es un indice en el pool de valores de la funcion.
     * El valor 0xFFFFFFFF indica "sin valor" (instrucciones void).
     */
    using IrValueId = uint32_t;
    static constexpr IrValueId IR_NO_VALUE = 0xFFFFFFFFu;

    /**
     * @brief Identificador de un bloque basico.
     */
    using IrBlockId = uint32_t;
    static constexpr IrBlockId IR_NO_BLOCK = 0xFFFFFFFFu;

    /**
     * @brief Descriptor de un valor SSA.
     *
     * Cada %nombre en el texto corresponde a un IrValue con un id unico.
     * Los parametros de funcion son valores especiales con is_param=true.
     */
    struct IrValue {
        IrValueId   id        = IR_NO_VALUE; ///< identificador unico (indice en IrFunction::values)
        IrType      type      = IrType::I64; ///< tipo del valor
        std::string name;                    ///< nombre legible ("%0", "%result", "%a", ...)
        bool        is_param  = false;       ///< true si es un parametro de funcion
        bool        is_const  = false;       ///< true si es una constante literal
        /// true si el valor (debe ser PTR) apunta a memoria HOST (e.g. retorno
        /// de @c rawalloc).  Los LOAD/STORE consultan este bit para decidir
        /// entre @c mov [rp] (s=0, memoria VM) y @c movh [rp] (s=1, memoria
        /// host).  La aritmetica de punteros y subscript propagan el bit
        /// desde el operando base.
        bool        is_host_ptr = false;
        /// Limitacion A (cerrada): true si el valor es un PTR a memoria VM
        /// (tipicamente la direccion de un slot ALLOCA en el stack del
        /// proceso) cuyo CONTENIDO es a su vez un host_ptr.  Lo setea el
        /// lowering en (a) @c write_local cuando se escribe un valor con
        /// @c is_host_ptr=true a un local address-taken, y (b) en @c &x
        /// cuando @c x es local host-bearing (caso indirecto via address-of).
        /// El emisor IR (case LOAD en @c ir_emitter.cpp) lo consulta para
        /// propagar @c is_host_ptr=true al SSA value resultante del LOAD,
        /// manteniendo la cadena de host_ptr a traves del round-trip
        /// @c i32** pp = &p; **pp = v.  Solo cubre 1 nivel de indireccion;
        /// patrones con mas niveles (e.g. @c &pp) caen al modelo legacy.
        bool        pointee_is_host_ptr = false;
        /// A.32.fix - true si el valor es un host_ptr a un objeto GESTIONADO
        /// por el GC (instancia de clase Vex tipicamente).  El emisor IR
        /// usa este flag para que cualquier @c push/@c pop alrededor de un
        /// CALL que pueda disparar GC se haga sobre el GcHandle (estable),
        /// no sobre el host_ptr (movido por evacuacion en GC generacional).
        ///
        /// Patron emitido al spillar:
        /// @code
        ///   gchandle reg, reg     // host_ptr -> GcHandle
        ///   push reg
        ///   ... call ...
        ///   pop reg
        ///   gcderef cur0, reg     // GcHandle -> host_ptr (refrescado tras GC)
        ///   xchg cur0, reg
        /// @endcode
        ///
        /// Coste: 4 instrucciones extra por spill, solo cuando aplica.
        /// Sin esto, ctors que invocan otra alocacion intermedia (e.g.
        /// @c this.field = new Inner(x)) ven @c this como host_ptr stale
        /// tras un minor GC -> escritura en memoria liberada -> segfault.
        bool        is_gc_object = false;
        uint64_t    const_val = 0;           ///< valor si is_const == true
    };

    // =========================================================================
    //  Instrucciones SSA
    // =========================================================================

    /**
     * @brief Par (valor, bloque) para los argumentos de una instruccion Phi.
     */
    struct IrPhiArg {
        IrValueId value;   ///< valor que llega desde el bloque predecesor
        IrBlockId block;   ///< bloque predecesor
    };

    /**
     * @brief Una instruccion SSA.
     *
     * Representacion plana: todos los campos de todas las instrucciones posibles.
     * La seleccion de campos activos depende de IrOp:
     *
     *   CONST:         dst, type, imm
     *   ADD..SAR:      dst, type, operands[0], operands[1]
     *   NEG/NOT/FNEG/FABS/FSQRT: dst, type, operands[0]
     *   CMP_*:         dst, type=BOOL, operands[0], operands[1]
     *   FCMP_*:        dst, type=BOOL, operands[0], operands[1]
     *   CAST/ZEXT/SEXT/TRUNC/ITOF/UITOF/FTOI/FTOUI/F32TOF64/F64TOF32/BITCAST:
     *                  dst, type, operands[0]
     *   BR:            target_block
     *   BR_COND:       operands[0]=cond, target_block=true_bb, false_block
     *   RET:           operands[0] si no es void
     *   PHI:           dst, type, phi_args[]
     *   CALL/TAILCALL: dst, func_name, operands[]
     *   CALLIND:       dst, func_ptr, operands[]
     *   CALLVIRT:      dst, operands[0]=obj, imm=vtbl_idx, operands[1..]=args
     *   CALLN:         dst, func_name (formato "lib:func"), operands[]
     *   ALLOCA:        dst, type, imm=count
     *   LOAD:          dst, type, operands[0]=ptr
     *   STORE:         operands[0]=val, operands[1]=ptr
     *   MEMCPY:        operands[0]=dst_ptr, operands[1]=src_ptr, operands[2]=len
     *   NEWOBJ:        dst, operands[0]=class_ptr
     *   GETFIELD:      dst, type, operands[0]=obj, imm=field_idx
     *   SETFIELD:      operands[0]=obj, imm=field_idx, operands[1]=val
     *   INSTANCEOF/CHECKCAST: dst, operands[0]=obj, operands[1]=class_ptr
     *   ISNULL/UNWRAP: dst, operands[0]=src
     *   SPECIALIZE:    dst, operands[0]=class_ptr, operands[1]=types_arr, imm=count
     *   GEP:           dst(marker), operands[0]=handle, imm=byte_offset (cur0 apunta al campo)
     *   GCWB_IR:       operands[0]=handle
     *   ARRAY_ALLOC:   dst, type=elem_type, operands[0]=len
     *   ARRAY_LEN:     dst, operands[0]=arr_vm_addr
     *   ARRAY_LOAD:    dst, type=elem_type, operands[0]=arr_vm_addr, operands[1]=idx
     *   ARRAY_STORE:   type=elem_type, operands[0]=arr_vm_addr, operands[1]=idx, operands[2]=val
     *   GCDEREF_IR:    operands[0]=handle (gcderef a cur0; usar solo seguido de readcur/writecur)
     *   STRMAKE:       dst, operands[0]=buf_vm_addr, operands[1]=len, imm=enc
     *   STRLEN:        dst, operands[0]=str_handle
     *   STRCAT:        dst, operands[0]=a_handle, operands[1]=b_handle
     *   STRCMP:        dst, operands[0]=a, operands[1]=b
     *   STRSLICE:      dst, operands[0]=str, operands[1]=range
     *   STRFLAT:       dst, operands[0]=str
     *   STRHASH:       dst, operands[0]=str
     *   STRINTERN:     dst, operands[0]=str
     *   STRRAW:        dst, operands[0]=str
     *   STRCONV:       dst, operands[0]=str, imm=enc
     *   STRRESERVE:    dst, operands[0]=cap_bytes
     *   STRFINALIZE:   operands[0]=str, operands[1]=new_len
     *   THROW:         operands[0]=exc_obj
     *   TRYENTER:      operands[0]=handler_pc, operands[1]=class_ptr
     *   TRYLEAVE:      (sin operandos)
     *   LANDINGPAD:    dst, type
     *   FUTURE:        dst
     *   AWAIT:         dst, type, operands[0]=future_handle
     *   FULFILL:       operands[0]=future_handle, operands[1]=value
     *   REJECT:        operands[0]=future_handle, operands[1]=error_code
     *   MSGSEND:       dst, operands[0]=pid, operands[1]=buf_addr, operands[2]=len
     *   MSGRECV:       dst, type, operands[0]=max_len, operands[1]=buf_addr
     *   RSPAWN:        dst, operands[0]=node_idx, operands[1]=fn_addr
     *   MONENTER/MONEXIT/MONWAIT/MONNOTI/MONNOTA: operands[0]=obj
     *   GETPROC/GETVM/GETMGR: dst
     *   SPAWN:         dst, operands[0]=fn_ptr
     *   RESUME:        operands[0]=pid
     *   SWAPCTX:       operands[0]=dst_ctx, operands[1]=src_ctx
     *   RAW_ASM:       func_name=texto_ensamblador (sin dst, sin operandos, nunca optimizado)
     */
    struct IrInstr {
        IrOp    op;          ///< operacion
        IrType  type;        ///< tipo del resultado (VOID si sin resultado)
        IrValueId dst;       ///< registro destino (IR_NO_VALUE si sin resultado)

        std::vector<IrValueId> operands;  ///< operandos de la instruccion

        uint64_t imm;           ///< literal para CONST, ALLOCA, GETFIELD, SETFIELD, CALLVIRT

        std::string func_name;  ///< para CALL/CALLN/TAILCALL: nombre de la funcion
        IrValueId   func_ptr;   ///< para CALLIND: id del valor con el puntero de funcion

        IrBlockId target_block;    ///< destino de BR o rama true de BR_COND
        IrBlockId false_block;     ///< rama false de BR_COND

        std::vector<IrPhiArg> phi_args;  ///< para PHI

        uint32_t source_line;  ///< numero de linea del fuente original (0 = desconocido)

        /// Si true, esta instruccion NO debe ser eliminada por copy_prop
        /// ni DCE.  Util para barreras de codegen como los MOVs que el
        /// lower_for/lower_while inserta antes del back-edge para
        /// proteger los SSA values de loop-carry contra el "live hole"
        /// del linear scan (ver lower_for / lower_while).
        bool     preserve = false;

        IrInstr() : op(IrOp::NOP), type(IrType::VOID), dst(IR_NO_VALUE),
                    imm(0), func_ptr(IR_NO_VALUE),
                    target_block(IR_NO_BLOCK), false_block(IR_NO_BLOCK),
                    source_line(0) {}
    };

    // =========================================================================
    //  Bloque basico
    // =========================================================================

    /**
     * @brief Bloque basico de la CFG (Control Flow Graph).
     *
     * Un bloque basico es una secuencia lineal de instrucciones con una
     * sola entrada y una sola salida.  La ultima instruccion es siempre
     * un terminador: BR, BR_COND, RET o UNREACHABLE.
     */
    struct IrBlock {
        IrBlockId              id;      ///< identificador unico (indice en IrFunction::blocks)
        std::string            name;    ///< nombre legible ("entry", "loop_body", ...)
        std::vector<IrInstr>   instrs;  ///< instrucciones en orden
        std::vector<IrBlockId> preds;   ///< bloques predecesores (para consistencia de Phi)
        std::vector<IrBlockId> succs;   ///< bloques sucesores
    };

    // =========================================================================
    //  Funcion SSA
    // =========================================================================

    /**
     * @brief Funcion completa en forma SSA.
     *
     * Contiene el grafo de bloques basicos y el pool de valores.
     * El primer bloque (id=0) es siempre el bloque de entrada "entry".
     */
    struct IrFunction {
        std::string              name;        ///< nombre calificado ("com.pkg.Foo.add")
        IrType                   ret_type;    ///< tipo de retorno
        std::vector<IrValueId>   params;      ///< IDs de los valores parametro
        std::vector<IrValue>     values;      ///< pool de todos los valores SSA
        std::vector<IrBlock>     blocks;          ///< bloques basicos (bloques[0] = entry)
        bool                     is_native   = false; ///< true si es stub para funcion nativa
        bool                     is_variadic = false; ///< true si acepta argc variable

        /**
         * @brief Crea un nuevo valor SSA en el pool.
         * @param type Tipo del valor.
         * @param name Nombre opcional (si vacio se genera "%%N").
         * @return ID del nuevo valor.
         */
        IrValueId new_value(IrType type, const std::string &name = "");

        /**
         * @brief Crea un nuevo bloque basico.
         * @param name Nombre del bloque (si vacio se genera "bbN").
         * @return ID del bloque.
         */
        IrBlockId new_block(const std::string &name = "");

        /**
         * @brief Anade una instruccion al bloque indicado.
         * @param block_id Bloque destino.
         * @param instr    Instruccion a anadir.
         */
        void append(IrBlockId block_id, IrInstr instr);
    };

    // =========================================================================
    //  Modulo IR
    // =========================================================================

    /**
     * @brief Modulo IR: coleccion de funciones con declaraciones globales.
     *
     * Unidad de compilacion de la SSA IR.  Corresponde a un archivo fuente
     * del HLL o a un modulo de VestaVM (@Module).
     */

    /**
     * @brief Definicion de un espacio de direcciones para el modulo .vel generado.
     *
     * Corresponde a la directiva `@space` del formato .ir, que el emisor
     * traduce a `@SpaceAddress { @Name, @IniAddress, @EndAddress }` en el .vel.
     */
    struct IrSpaceDef {
        std::string name;         ///< nombre del espacio (p.ej. "anonymous")
        uint64_t    ini_address;  ///< direccion inicial
        uint64_t    end_address;  ///< direccion final
    };

    /**
     * @brief Definicion de una seccion para el modulo .vel generado.
     *
     * Corresponde a la directiva `@section` del formato .ir, que el emisor
     * traduce a `@Section { @Name, @SpaceAddress, @Align }` en el .vel.
     */
    struct IrSectionDef {
        std::string name;       ///< nombre de la seccion (p.ej. "code")
        std::string space_name; ///< espacio de direcciones al que pertenece
        uint64_t    align;      ///< alineacion en bytes (p.ej. 0x1000)
    };

    /**
     * @brief Importacion de una funcion nativa desde una libreria dinamica.
     *
     * Corresponde a un bloque @c "@Method { @Lib(\"...\") @Name(\"...\") }" dentro
     * del bloque @c @Import del .vel.  El frontend (Vex u otro) registra una
     * IrNativeImport por cada funcion nativa que sus llamadas (CALLN) van a
     * usar.  El emisor agrupa todas en un unico bloque @Import.
     */
    struct IrNativeImport {
        std::string lib;   ///< Ruta logica de la libreria (p.ej. "stdlib/native/io/vesta_io")
        std::string name;  ///< Nombre de la funcion nativa (p.ej. "vio_println")
    };

    struct IrModule {
        std::string                              name;       ///< nombre del modulo (@module)
        std::vector<IrFunction>                  functions;  ///< funciones definidas
        std::vector<std::string>                 imports;    ///< nombres de funciones importadas (@import)
        std::unordered_map<std::string, IrValueId> globals;  ///< variables globales
        std::vector<std::string>                 native_libs; ///< libs nativas (@native_lib)

        /**
         * @brief Datos estaticos del modulo: cada entrada es la imagen de bytes
         *        de un literal de cadena u otro blob inmutable.
         *
         * El emisor IR genera al final del .vel una etiqueta @c s_<i> por cada
         * entrada con la directiva @c db ... bytes.  El opcode @c STR_LIT_ADDR
         * con @c imm=i carga la direccion VM de @c s_i en un registro.
         *
         * Layout: @c std::vector<std::vector<uint8_t>> garantiza memoria
         * contigua por entrada (cache-friendly al iterar) y permite que
         * cualquier byte (incluido @c '\0') aparezca en el contenido.
         */
        std::vector<std::vector<uint8_t>> static_data;

        /**
         * @brief Funciones nativas que el modulo declara importar.
         *
         * Cada una corresponde a un @c @Method dentro del bloque @c @Import
         * del .vel emitido.  El emisor las agrupa en un unico bloque para
         * evitar declaraciones redundantes; el frontend solo debe garantizar
         * que cada par (lib, name) usado por un CALLN aparece aqui al menos
         * una vez (los duplicados los filtra el emisor).
         */
        std::vector<IrNativeImport> native_imports;

        // Metadatos de compilacion (opcionales; el emisor genera valores por defecto si estan vacios)
        std::string                format;    ///< formato de salida: "velb" (defecto) u otro
        std::vector<IrSpaceDef>    spaces;    ///< espacios de direcciones (@space)
        std::vector<IrSectionDef>  sections;  ///< secciones de codigo (@section)

        /**
         * @brief Anade una funcion al modulo.
         * @param fn Funcion a anadir.
         * @return Indice de la funcion en el modulo.
         */
        size_t add_function(IrFunction fn);

        /**
         * @brief Registra un literal de cadena en static_data.
         *
         * Si el contenido ya existe lo deduplica devolviendo el indice
         * existente, evitando datos duplicados en el binario final.
         *
         * @param bytes Contenido literal (puede contener nuls intermedios).
         * @return Indice estable que el lowering puede pasar a STR_LIT_ADDR.
         */
        uint64_t intern_static_data(std::vector<uint8_t> bytes);

        /**
         * @brief Registra una importacion nativa, deduplicando.
         *
         * Si la pareja (lib, name) ya esta en native_imports no se anyade
         * de nuevo; el lowering puede llamarlo libremente desde cualquier
         * punto sin preocuparse de duplicados.
         */
        void register_native_import(std::string lib, std::string name);
    };

    // =========================================================================
    //  Serializado / deserializado de texto
    // =========================================================================

    /**
     * @brief Imprime un IrModule al formato de texto SSA IR.
     *
     * @param mod Modulo a serializar.
     * @param out Stream de salida.
     */
    void ir_print(const IrModule &mod, std::ostream &out);

    /**
     * @brief Parsea un archivo .ir y construye un IrModule.
     *
     * @param text  Contenido del archivo .ir como cadena.
     * @param out   Modulo de salida.
     * @param error Mensaje de error (vacio si no hay error).
     * @return true si el parseo fue exitoso.
     */
    bool ir_parse(const std::string &text, IrModule &out, std::string &error);

    // =========================================================================
    //  Verificador
    // =========================================================================

    /**
     * @brief Verifica que el modulo esta en forma SSA correcta.
     *
     * Comprueba:
     *   - Cada valor se define exactamente una vez.
     *   - Cada bloque termina en un terminador.
     *   - Los tipos de los operandos son consistentes con el opcode.
     *   - Los operandos referenciados existen en el pool.
     *
     * @param mod    Modulo a verificar.
     * @param errors Vector de mensajes de error encontrados.
     * @return true si el modulo es valido.
     */
    bool ir_verify(const IrModule &mod, std::vector<std::string> &errors);

} // namespace ir

// Restaurar las macros de Windows que anulamos al principio del header.
#ifdef _WIN32
#  pragma pop_macro("VOID")
#  pragma pop_macro("CONST")
#  pragma pop_macro("BOOL")
#endif

#endif /* SSA_IR_H */
