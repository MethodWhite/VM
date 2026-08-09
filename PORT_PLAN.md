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
- **Pendiente**: runtime FULL/EMBED para I/O/GC/strings (los vrt_* que el
  runtime linkado aporta), multi-ISA, DWARF.
- Ver `src/aot/README.md` limitaciones.

### vectorize (P1, 30 commits)
- Desmon: `src/vx/vectorize.cpp` (2359 LOC) — pase del Lowering que
  reconoce loops `dst[i]=f(src[i])` y baja a VEC ops.
- Requiere: IR ops `VEC_UNOP/BINOP/FMA/BCAST/FMA_S` + codegen (JIT SIMD)
  + `jit/vec_isa.h` (ancho SSE2/AVX2/AVX512).
- Port: mapear el pase a `vex::Lowering` (los métodos ya se declaran en
  `include/vex/lowering.h`), añadir VEC ops al `ssa_ir.h` local y su
  emisión en `ir_emitter.cpp`/`x86_encoder.cpp`.

### ASA (P2, 37 commits)
- Local: `src/vex/asm_parser.cpp` + `asm_lowering.cpp` (RAW_ASM opaco).
- Desmon: `src/vx/asm/*` (20 archivos, ~189KB) — `asm_effects.h` (tabla
  plana por-instrucción), `asm_analyze.h` (efectos por bloque),
  `asm_lift*` (lifting a IR tipado), CFG, atómicos, SIMD.
- Port en fases:
  1. `asm_effects` + `asm_analyze` (análisis, sin IR nuevo) — autónomo.
  2. `asm_lift` (requiere IR ops `ASM_MICRO`, `ATOMIC_*`, `VEC_*`).

## Criterio de "hecho" por módulo

- Compila integrado en `vm` (no archivo huérfano).
- Tests del módulo (unit + e2e Vex) pasan.
- Sin regresión en la suite existente (ctest ≥ 57/64).
- Documentado (comentario de cabecera del módulo).
