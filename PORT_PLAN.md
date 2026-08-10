# Plan de Port: trabajo de desmonHak a la rama local

Este documento cataloga el trabajo del autor original (desmonHak, rama
`origin/feature`, 1446 commits) que la rama local (`fix/ci-lint-bug`)
NO tiene, y la estrategia para portarlo.

> **Estado**: 2026-08-09.  El port es por **reimplementación**, no
> cherry-pick: las arquitecturas divergieron (Desmon: `src/vx/`,
> local: `src/vex/`; Desmon: `rbank` regalloc, local: `linear_scan`).

## Principio

Cada subsistema se porta a la infraestructura local (`vex::` en vez de
`vx::`, IR ops y runtime locales), verificando con tests tras cada
módulo.  No se arrastra código muerto de la otra rama.

## Subsistemas por prioridad

### P1 — Rendimiento del lenguaje (mayor valor/esfuerzo)

| Subsistema | Commits | Alcance | Estado |
|------------|---------|---------|--------|
| **AOT completo** | 160 | AOT multi-ISA (x86/arm64), ELF/PE, GC AOT, debug info | local tiene solo `src/aot/` rudimentario (ELF x86-64, sin driver) |
| **vectorize** | 30 | Auto-vectorización de loops → VEC ops (SIMD) + codegen | IR ops VEC_* ausentes en local |
| **comptime** | 19 | Metaprogramación compile-time avanzada | local tiene `comptime_introspect.cpp` |

### P2 — Lenguaje y semántica

| Subsistema | Commits | Alcance | Estado |
|------------|---------|---------|--------|
| **ASA / inline asm** | 37 | Análisis de efectos de `asm {}`, lifting a IR tipado, CFG, atomics, SIMD | local tiene asm básico (RAW_ASM opaco) |
| **Vex/lenguaje** | 94+21 | Nuevas features del lenguaje | verificar feature por feature |
| **import/packaging** | 23 | Sistema de importación/módulos | local tiene `module_interop.cpp` |
| **wideint** | 13 | Enteros de ancho arbitrario | stdlib |

### P3 — Herramientas y diagnóstico

| Subsistema | Commits | Alcance | Estado |
|------------|---------|---------|--------|
| **vxdbg / diagnóstico** | 24+14 | Debug info nativa, trazas, `--explain` | local tiene `src/debug/` + el JIT line-map que portamos |
| **LSP** | 8 | Servidor de lenguaje avanzado | local tiene `src/lsp/json_rpc.cpp` básico |
| **GC** | 16 | GC avanzado | local tiene `src/gc/` completo |

## Notas por módulo

### AOT (P1, 160 commits)
- Local: `src/aot/aot_compiler.cpp` (550 LOC, ELF x86-64, sin driver ni
  flag `--debug-info`).  Se compila pero nadie lo invoca.
- Desmon: AOT multi-ISA con `src/toolchain/`, `src/aot/` (14+ archivos),
  linker propio, ELF/PE, GC AOT, debug info.
- **Hecho (2026-08-09)**:
  - Driver `vm --aot prog.vex -o prog [--aot-tier full|embed|bare]`
    (main.cpp): .vex -> compile_vex_source -> ir_section_bytes ->
    parse_ir_section (IrModule) -> aot::AotCompiler -> ELF.
  - Fix de relocacion interna `_start -> main` en ejecutables BARE
    (el call main apuntaba a si mismo -> exit 0).
  - Resolver de CALLs a funciones user (resolve_user_fn): las llamadas
    entre funciones se resuelven por direccion absoluta (0x400000+offset).
  - Self-recursion: el callee en compilacion devuelve 0x400000+offset
    actual (current_fn_name_).
  - **Verificado**: `return 42` -> 42; `if` -> 7; `fib(10)` -> 55;
    `factorial(5)` -> 120; `while` loop (0..4) -> 10.  El AOT BARE ya
    compila programas con control de flujo, llamadas y recursion.
- **Tier FULL/EMBED (2026-08-09)**: relocaciones R_X86_64_64 para los
  CALLs entre funciones.  El selector marca los imm64 de user-calls, el
  encoder registra sus posiciones, y el AOT genera relocaciones al
  simbolo destino.  El .o se linka con g++ contra vmcore/vex_lib/vpp_lib.
  **Verificado**: FULL ejecuta return 42, if=7, fib(10)=55, factorial=120,
  loop=10, gcd=6.
- **Pendiente**: runtime FULL/EMBED para I/O/GC/strings.  El AOT linka
  libvmcore pero los plugins de E/S (vesta_io.so, etc.) se cargan
  dinamicamente y el AOT no los resuelve: STR_LIT_ADDR (string literals)
  y calln (FFI) fallan en el AOT.  Requiere embeber los plugins o portar
  el runtime de E/S de Desmon (subsistema aparte).
- **Pendiente**: multi-ISA, DWARF.
- Ver `src/aot/README.md` limitaciones.

### vectorize (P1, 30 commits)
- Desmon: `src/vx/vectorize.cpp` (2359 LOC) — pase del Lowering que
  reconoce loops `dst[i]=f(src[i])` y baja a VEC ops.
- Requiere: IR ops `VEC_UNOP/BINOP/FMA/BCAST/FMA_S` + codegen (JIT SIMD)
  + `jit/vec_isa.h` (ancho SSE2/AVX2/AVX512).
- **Estado**: el memcpy idiom local (`ir_pass_loop_memcpy_idiom`) cubre la
  copia host->host.  Se probo portar el idiom a nivel AST (lowering) pero
  la semantica de `vmcopy` local es VM->host (curN = host dst, src = dir
  VM), NO host->host: el idiom de Desmon asume ambos host.  Portar requiere
  ajustar la semantica de MEMCPY/vmcopy en el runtime (trabajo del runtime,
  no del frontend).  Las VEC ops SIMD requieren backend SIMD (pendiente).

### import/packaging (P2, 23 commits)
- Los fixes de módulos de Desmon (structs importados con métodos, enums
  con valor) requieren que el `StructLayout` local soporte métodos, que
  hoy no tiene (los structs Vex locales no serializan métodos en `.vxi`).
  Es un cambio del type checker/lowering, no un port directo.

### vxdbg / diagnóstico (P3, 24+14 commits)
- El `--explain` de Desmon requiere el acompañante `.vxdbg` del AOT con
  debug info (sistema de grafo semántico del binario).  Port grande;
  el local tiene `src/debug/debug_info.cpp` + el JIT line-map portado.

### ASA / inline asm (P2, 37 commits)
- Local: `src/vex/asm_parser.cpp` + `asm_lowering.cpp` (RAW_ASM opaco).
- Desmon: `src/vx/asm/*` (20 archivos, ~189KB) — `asm_effects.h` (tabla
  plana por-instrucción), `asm_analyze.h` (efectos por bloque),
  `asm_lift*` (lifting a IR tipado), CFG, atómicos, SIMD.
- **Hecho (2026-08-09)**:
  - `asm_effects` (tabla mnemonic->efectos x86/arm64, inferencia de
    clobbers, canonicalizacion de registros, normalizacion de numeros).
  - `asm_analyze` (modelo de efectos por bloque).
  - `instr_db` + tablas generadas (11 archivos): DB embebida por ISA.
  - `asm_lift` (reconocimiento de atomicos: lock cmpxchg -> ATOMIC_CAS,
    lock xadd -> ATOMIC_ADD) + `asm_lift_emit` (emision tipada).
  - Integrado en el lowering: los patrones atomicos se liftan a
    ATOMIC_CAS_I64/ATOMIC_ADD_I64; los clobbers se infieren del cuerpo.
  - Tests: `test_asm_effects.cpp` (11) + `test_asm_lift.cpp` (9) +
    `test_asm_lift_micro.cpp` (5).
- **Hecho (IR op ASM_MICRO)**: IrOp::ASM_MICRO + AsmMicro/AsmMicroOperand/
  AsmOperandFlag/AsmRegBinding en el IR local, emitter que re-emite el
  tmpl con los registros, y lifting micro completo (asm_lift_micro/
  asm_lift_x86/asm_lift_general/asm_phys_reg) habilitado.  El asm se
  modela como IR con la DB (efectos/timing), el regalloc asigna, el
  backend re-emite verbatim.

## Criterio de "hecho" por módulo

- Compila integrado en `vm` (no archivo huérfano).
- Tests del módulo (unit + e2e Vex) pasan.
- Sin regresión en la suite existente (ctest ≥ 57/64).
- Documentado (comentario de cabecera del módulo).

## Features del lenguaje (brechas vs Desmon) y progreso

### Hecho (2026-08-09)
- **Dunder operator overloading** (`__add__`/`__eq__`/`__ne__` derivado/
  etc.): AST `overload_method`, type checker mapea BinOp->__op__, lowering
  sintetiza `lhs.__op__(rhs)`.  Verificado: Vec2 `+`=42, `==`, `!=`.
  Nota: los structs locales no tienen metodos -> el dunder solo aplica a
  CLASS; StructLayout con metodos es un port posterior.

### Pendiente por valor
- **Enteros de ancho arbitrario** (u128..u512): requiere `union`,
  `@Abstract` (structs base) y constructores comptime
  (`std.comptime.literal`: IntLit/parse_int_lit).
- **Extension methods + impl blocks**: programar tipos ajenos.
- **Variádicos** (`T... rest`), **valued enums C-style**, **unions**,
  **`cfn`**, **`gc<T>`**, **`thread_local`**.
- **Concepts + bounds de genéricos**.
- **`@overlay struct`**, match sobre enteros/strings, `bytes{}`.

### Optimizaciones de rendimiento pendientes
- **FMA cross-backend** (`ir_pass_fuse_fma`) — 1 redondeo en vez de 2.
- **Auto-PGO tier-2** (contadores de branch + if-conversion).
- **Banco FP ZMM + vectorización VEC_*** (fp_jit -30%, array_sum -18%).
- **Fast-path threaded del interp** (mld/mst/shifts ~8x).
