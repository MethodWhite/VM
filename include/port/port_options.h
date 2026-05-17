/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file port_options.h
 * @brief Configuracion del transpiler IR -> codigo fuente (C, Java, JS, ...).
 *
 * Las opciones aqui controlan COMO se baja el SSA IR a un lenguaje destino:
 *   - Modelo de memoria (sin GC, malloc, GC vesta_rt, GC Boehm).
 *   - Manejo de excepciones (sin EH, setjmp/longjmp, codigos de retorno).
 *   - Estilo de tipos (stdint.h portatil vs tipos builtin del lenguaje).
 *   - Optimizaciones hint (atributos del compilador host).
 *
 * Cada backend (CBackend, JavaBackend, ...) consulta estas opciones para
 * decidir QUE codigo emitir.  El transpiler_base es agnostico al lenguaje
 * destino; solo orquesta CFG, PHI lowering y dispatch sobre el backend.
 *
 * Politica de defaults (acordado con el usuario, 2026-05-15):
 *   - GC = None       (programacion de sistemas; malloc/free por defecto).
 *   - Exc = SetJmp    (portable, ~3 ns overhead aceptable).
 *   - Types = StdInt  (uint64_t, int32_t, ... portatil entre toolchains).
 *
 * Si un programa Vex usa NEW/closures/strings (que requieren GC), el usuario
 * debe pasar @c --port-gc=vesta o @c --port-gc=boehm explicitamente.  En
 * caso contrario, esas IR ops se emiten como comentarios "unsupported".
 */

#ifndef PORT_OPTIONS_H
#define PORT_OPTIONS_H

#include <string>

namespace port {

    /**
     * @brief Politica de gestion de memoria del codigo generado.
     *
     * Determina como se traducen las IR ops de alocacion y liberacion:
     *   - @c RAW_ALLOC / @c RAW_FREE  -> SIEMPRE malloc/free (independiente del modo).
     *   - @c NEWOBJ / @c GC_ALLOC -> depende del modo elegido.
     */
    enum class GcMode {
        /// Sin GC.  Las IR ops de objetos GC (NEWOBJ, etc.) emiten comentario
        /// "unsupported" y un stub.  El programa generado solo usa malloc/free
        /// y se enlaza sin libreria de runtime adicional.  Es la opcion por
        /// defecto para programacion de sistemas.  Tambien activa los flags
        /// @c -fno-exceptions @c -fno-rtti recomendados al compilar.
        None,

        /// Usa @c vesta_rt como libreria runtime.  Cada NEWOBJ baja a
        /// @c vrt_gc_alloc(size, class_ptr) y la convencion de ObjectHeader
        /// se mantiene identica al interp/JIT.  Requiere enlazar contra
        /// @c vesta_rt.lib en la compilacion del codigo generado.
        Vesta,

        /// Usa el GC conservativo de Boehm (libgc).  Cada NEWOBJ baja a
        /// @c GC_malloc(size).  Mas lento que Vesta_rt pero compatible con
        /// codigo C existente.  Requiere enlazar contra @c -lgc.  No
        /// implementado en v1 (placeholder para futuro).
        Boehm,
    };

    /**
     * @brief Politica de manejo de excepciones.
     *
     * Vex/IR usa @c TRYENTER + @c THROW + @c LANDINGPAD para EH.  El backend
     * debe escoger una representacion en el lenguaje destino:
     */
    enum class ExcMode {
        /// Sin EH.  Las IR ops @c THROW emiten @c abort() o
        /// @c __builtin_unreachable().  @c TRYENTER/TRYLEAVE son no-op.
        /// Util para programas que NO usan try/catch.  Cero overhead.
        None,

        /// setjmp/longjmp.  Cada funcion con @c TRYENTER declara un
        /// @c jmp_buf local; @c THROW hace @c longjmp.  Portable POSIX +
        /// Windows.  Overhead ~3 ns por setjmp en hot path (compilador
        /// puede eliminar si no se usa).  Default v1.
        SetJmp,

        /// Codigo de retorno + parametro hidden.  Cada funcion recibe un
        /// @c int* __exc_out adicional; @c THROW asigna codigo + return.
        /// Mas lento que setjmp en codigo SIN excepciones (cada call
        /// inspecciona el codigo) pero mas determinista en ARM/embedded
        /// donde setjmp es caro.  No implementado en v1.
        ReturnCode,
    };

    /**
     * @brief Modo de instrumentacion para debugging del codigo generado.
     */
    enum class InstrumentMode {
        None,    ///< Sin instrumentacion (default).
        Trace,   ///< Emite @c vex_trace_enter/exit en cada funcion.  Imprime
                 ///< a stderr con indentacion por depth.  Util para ver el
                 ///< flow de ejecucion en debugging.
        Profile, ///< Emite @c vex_profile_enter/exit que miden tiempo
                 ///< (rdtsc o @c clock_gettime) por funcion.  Al exit
                 ///< del programa imprime estadisticas por funcion:
                 ///< count, total_ns, avg_ns.
    };

    /**
     * @brief Politica de strings en el codigo generado.
     */
    enum class StringMode {
        /// Strings como @c const @c char* crudo (literales en @c .rodata),
        /// sin runtime helpers.  Las ops dinamicas (STRMAKE, STRCAT, STRLEN
        /// que no sea sobre literales) emiten stubs "unsupported".  Util
        /// cuando el programa solo usa strings literales para FFI.  Cero
        /// runtime, cero alocacion.
        Raw,

        /// Strings via mini-runtime @c VexString embebido al inicio del .c:
        ///   typedef struct { uint32_t byte_len; uint32_t code_points; ... } VexString;
        /// Soporta @c STRMAKE/STRCAT/STREQ/STRLEN/STRRAW/STRGETBYTES con
        /// helpers @c vex_str_*.  Tracking per-thread de alocaciones via
        /// linked list; warning al exit del thread si quedan blocks vivos.
        /// Default v1 (cubre el 99% de programas Vex con strings).
        Managed,
    };

    /**
     * @brief Estilo de tipos en el codigo generado.
     */
    enum class TypeStyle {
        /// Usa los typedef portables de @c stdint.h:
        ///   I8/U8   -> int8_t / uint8_t
        ///   I16/U16 -> int16_t / uint16_t
        ///   I32/U32 -> int32_t / uint32_t
        ///   I64/U64 -> int64_t / uint64_t
        ///   F32/F64 -> float / double (C tipos builtin)
        ///   PTR     -> void*
        ///   HANDLE  -> uint32_t (GcHandle)
        ///   BOOL    -> _Bool (C99)
        /// Recomendado: garantiza tamanos exactos cross-platform.
        StdInt,

        /// Usa tipos builtin del lenguaje destino: en C esto seria
        /// @c char/short/int/long/long long con sus reglas de promocion.
        /// NO recomendado.  Solo para output legible cuando el codigo
        /// final no necesita corre exactamente en plataformas mixtas.
        Builtin,
    };

    /**
     * @brief Configuracion completa del transpiler IR -> codigo destino.
     */
    struct PortOptions {
        GcMode     gc        = GcMode::None;       ///< Default: no GC, malloc/free.
        ExcMode    exc       = ExcMode::SetJmp;    ///< Default: setjmp/longjmp.
        TypeStyle  types     = TypeStyle::StdInt;  ///< Default: stdint.h portatil.
        StringMode strings   = StringMode::Managed; ///< Default: VexString managed.
        InstrumentMode instrument = InstrumentMode::None; ///< Default: sin instrumentacion.

        /// Si true, emite comentarios verbose en el codigo generado:
        /// nombre IR de cada op, source_line del .vex original, anotaciones
        /// de los SSA values.  Util para debug del transpiler y para
        /// inspeccionar mapeo IR <-> output.  Coste: ~3x mas grande el output.
        /// Default: true (cuesta 0 si el usuario tiene @c gcc -O3 -s).
        bool emit_comments = true;

        /// Si true, emite atributos de optimizacion del compilador host:
        ///   @c __attribute__((hot))   para funciones con profile alto
        ///   @c __attribute__((cold))  para funciones de error/setup
        ///   @c __attribute__((always_inline))  para helpers triviales
        ///   @c __restrict__   en parametros @c host_ptr cuando no aliasing.
        /// Solo GCC/Clang.  Cero efecto en MSVC.
        /// Default: true (GCC los ignora si no aplica).
        bool emit_compiler_hints = true;

        /// Nombre del modulo de salida.  Se usa como prefijo del simbolo
        /// @c main_<module>() cuando el frontend Vex emite una funcion
        /// llamada "main" -- evita colision con el @c main() de C.  Si
        /// vacio, usa @c "vex" como fallback.
        std::string module_name;

        /// Path completo del archivo .vex original.  Se emite como comentario
        /// header del archivo generado (@c "// source: foo.vex") para que el
        /// usuario sepa que fuente produjo este output.  Vacio = sin header.
        std::string source_path;

        /// Si true, marca cualquier funcion no soportada por el backend
        /// (e.g. C2 JIT-specific markers como MAKE_CLOSURE, RAW_ASM)
        /// como FATAL en lugar de emitirla como stub.  Util para CI
        /// que quiere verificar que TODO el programa se baja sin gaps.
        /// Default: false (graceful: emit stub + warning a stderr).
        bool strict_unsupported = false;

        /// Modo @c freestanding: no se emite ningun @c #include <stdio.h>
        /// / @c <stdlib.h> / @c <math.h> automatico.  Los snippets de
        /// runtime que dependen de la libc hosted se omiten; el programador
        /// debe proveer en su codigo:
        ///   - @c VEX_NORETURN @c "void @c vex_throw(int)"
        ///   - @c vex_panic_with_str si usa try/catch
        ///   - Bridges propios para print/IO si quiere strings
        /// Usado para programacion de sistemas, bootloaders, kernels
        /// donde no hay libc disponible.
        bool freestanding = false;

        /// Directorio donde estan los snippets @c .v.c (relativo a vm.exe
        /// si vacio, o absoluto si comienza con / o letra de unidad).
        /// Default: vacio = autodetect via @c argv[0].
        std::string stdlib_port_c_dir;

        /// Politica de targeting de CPU para el codigo generado:
        ///   "" (default)   - portable, ningun pragma de CPU
        ///   "native"       - @c "#pragma GCC target(\"arch=native\")" mas
        ///                    flags agresivas (avx2,fma,bmi2 si soportadas)
        ///   "x86-64-v2"    - SSE4.2, baseline para Linux moderno
        ///   "x86-64-v3"    - AVX2 + FMA + BMI1/BMI2 (Haswell+)
        ///   "x86-64-v4"    - AVX-512 (Skylake-X+)
        /// El usuario tambien puede pasar @c -march=X al @c gcc al
        /// compilar el @c .c -- estos pragmas solo HINTEAN al optimizer.
        std::string arch_target;

        /// Cuando true, emite atributos agresivos:
        ///   - @c __attribute__((const)) en funciones puras detectadas
        ///   - @c __attribute__((cold)) en paths que solo throw/panic
        ///   - @c __builtin_expect en exception checks (rama unlikely)
        ///   - @c __builtin_unreachable() tras @c vex_throw/longjmp
        ///   - @c __restrict__ en TODOS los pointer params
        ///   - @c __attribute__((always_inline)) en accesors triviales
        /// Default: true (sin cambios en correctness, solo en perf).
        bool aggressive_opt = true;
    };

    /**
     * @brief Parsea @c GcMode desde string CLI ("none", "vesta", "boehm").
     * @param s Texto del flag.
     * @param out Resultado parseado.
     * @return true si valido.
     */
    bool parse_gc_mode(const std::string &s, GcMode &out);

    /**
     * @brief Parsea @c ExcMode desde string CLI ("none", "setjmp", "returncode").
     */
    bool parse_exc_mode(const std::string &s, ExcMode &out);

    /**
     * @brief Parsea @c TypeStyle desde string CLI ("stdint", "builtin").
     */
    bool parse_type_style(const std::string &s, TypeStyle &out);

    /**
     * @brief Parsea @c StringMode desde string CLI ("raw", "managed").
     */
    bool parse_string_mode(const std::string &s, StringMode &out);

    /**
     * @brief Parsea @c InstrumentMode desde string CLI ("none", "trace", "profile").
     */
    bool parse_instrument_mode(const std::string &s, InstrumentMode &out);

} // namespace port

#endif // PORT_OPTIONS_H
