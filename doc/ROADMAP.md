# Roadmap de VestaVM

Plan publico de fases A-H, desde el frontend Vex hasta ejecutables nativos
standalone.

> **Snapshot**: 2026-05-17.

---

## Indice

- [Roadmap de VestaVM](#roadmap-de-vestavm)
  - [Indice](#indice)
  - [1. Vision general de fases](#1-vision-general-de-fases)
  - [2. Phase A: Frontend Vex (completa)](#2-phase-a-frontend-vex-completa)
  - [3. Phase B: IR cleanups (completa)](#3-phase-b-ir-cleanups-completa)
  - [4. Phase C: libvesta\_rt extraction (parcial)](#4-phase-c-libvesta_rt-extraction-parcial)
  - [5. Phase D: JIT C1+C2 (en progreso)](#5-phase-d-jit-c1c2-en-progreso)
    - [Sub-hitos completados](#sub-hitos-completados)
    - [Sub-hitos pendientes](#sub-hitos-pendientes)
    - [Camino acelerado (solo JIT funcional, sin AOT/PGO completo)](#camino-acelerado-solo-jit-funcional-sin-aotpgo-completo)
    - [Camino completo (C1+C2+AOT+PGO + nativo standalone)](#camino-completo-c1c2aotpgo--nativo-standalone)
  - [6. Phase E: GC + safepoints + JIT integration (parcial)](#6-phase-e-gc--safepoints--jit-integration-parcial)
  - [7. Phase F: AOT + object emitter (pendiente)](#7-phase-f-aot--object-emitter-pendiente)
  - [8. Phase G: Native exception unwinding + debug (pendiente)](#8-phase-g-native-exception-unwinding--debug-pendiente)
  - [9. Phase H: ARM/AArch64 + Linux target (pendiente)](#9-phase-h-armaarch64--linux-target-pendiente)
  - [10. Vision a largo plazo](#10-vision-a-largo-plazo)
  - [Prioridades a corto plazo (proximos 3 meses)](#prioridades-a-corto-plazo-proximos-3-meses)
  - [Como contribuir al roadmap](#como-contribuir-al-roadmap)

---

## 1. Vision general de fases

```text
Phase A  -->  Frontend Vex completo                          [COMPLETO]
Phase B  -->  IR cleanups + monomorphization contract        [COMPLETO]
Phase C  -->  Extraer libvesta_rt como libreria standalone   [PARCIAL]
Phase D  -->  JIT C1+C2 (template + optimizing)              [COMPLETO: D.0-D.9]
Phase E  -->  Stackmaps + safepoints + GC integration        [PARCIAL: D.2 Fase 1+2]
Phase F  -->  AOT: object emitter (COFF/ELF) + linker propio [PENDIENTE]
Phase G  -->  Native exception unwinding + debug info        [PENDIENTE]
Phase H  -->  ELF backend + Linux/macOS native               [PENDIENTE]
Phase Z  -->  Memoria compartida cross-process (Erlang-style) [COMPLETO]
```

Tiempo estimado total para llegar al ejecutable nativo standalone: ~18 meses
adicionales full-time desde el estado actual (2026-05-17). Plan tecnico
detallado documentado en cada seccion de fase abajo + en los headers de
`include/jit/`, `include/vesta_rt/` y los comentarios doxygen de los modulos
correspondientes.

---

## 2. Phase A: Frontend Vex (completa)

**Status**: 200/200 e2e PASS. Cubre TODAS las features del lenguaje listadas
en [doc/LANGUAGE.md](./LANGUAGE.md).

Hitos cerrados (de mas significativo a menos):

- **A.1-A.3**: tipos primitivos, structs, punteros, arrays nativos,
  `malloc`/`free` con bit `is_host_ptr`.
- **A.4**: POO dinamica con reflexion + AOP via `defclass`/`deffield`/
  `defmethod`/`findclass`/`findmethod`/`addadvice` opcodes.
- **A.5**: herencia simple + interfaces + properties (get/set) + `final` +
  `static` + AOP (BEFORE/AFTER/AROUND con `proceed()`).
- **A.6**: try/catch/finally, foreach, **Optional/Result builtins SRET** con
  `Some`/`None`/`Ok`/`Err`/`isPresent`/`unwrap`/`isOk`/`value`/`error`,
  implicit Some, must-handle Result, `!!`, `nonnull T`, `T !!name`.
- **A.7**: `synchronized` con cleanup automatico (try/finally implicito),
  `spawn` + IPC (`pid`/`msgsend`/`msgrecv`), multi-thread real opcional
  (`--schedulers N`), spawn placement (`spawn here`/`spawn on(N)`),
  `@Async` con args + `Future<T>`, `rspawn` cross-node.
- **A.8**: Generics MVP via monomorphizacion compile-time.
- **A.9**: `loadmodule(path)` con rebase transparente (VERSION_VELB=0x2).
- **A.10-A.11**: Closures con captura lexica + ADTs + pattern matching.
- **A.12**: cierre de limitaciones (PHI fix, SRET enums, capture-by-ref).
- **A.13-A.16**: tests integradores, control flow C-style (break/continue/goto),
  reflexion completa (`newInstance`/`invoke`), I/O optimo, debugger TCP.
- **A.17-A.18**: `FatalError` capturable, AV recovery (`try/catch` capta
  segfaults de OS), tipo `string` dedicado.
- **A.20-A.22**: backend float real (FADD/FMUL/FSQRT con memory-roundtrip
  GP↔ZMM), conversiones `fextend`/`fnarrow`, compound assign sobre lvalues
  no-triviales, triple-quoted strings.
- **A.24-A.26**: FFI completo (`extern "lib.dll" { fn ... }` declarativo +
  `ffi_open`/`ffi_sym`/`ffi_call` runtime), STDLIB de colecciones
  hardware-optimized (SIMD swisstable HashMap), colecciones como tipos
  primitivos.
- **A.34**: Optimizaciones runtime (pool de FrameHeader, cache de ClassInfo*,
  GC stack scanning conservativo + interior scan), control flow goto/labels,
  reflexion runtime completa.
- **A.34.fix1-fix20**: bugs criticos cerrados (PHI liveness, interpolacion
  con tipos primitivos, AV recovery, etc.).
- **A.35-A.36**: Smart pointers `unique<T>`/`shared<T>` con `mvtake`,
  deleters custom, Tier 1 con SRET 16 bytes; **Borrow checker** compile-time
  con 4 reglas R1-R4 + 4 fases F1-F4 (NLL, OwnerKind, reborrow con suspend
  stack, lifetime elision).

**Performance del intérprete tras Phase A**: ~340 MIPS promedio.

---

## 3. Phase B: IR cleanups (completa)

**Status**: 100% completo, 2026-05-12. Total ~500 LOC reales (vs ~900 estimados).

- **B.1 (CERRADO)**: `IrOp::MAKE_CLOSURE` marker para captura/escape
  analisis futuro.
- **B.2 (CERRADO)**: `IrOp::MAKE_VARIANT` + `IrOp::MATCH_VARIANT` markers
  semanticos para que el C2 JIT haga case-splitting eficiente sobre enums.
- **B.3 (CERRADO)**: Monomorphization contract via
  `IrFunction::generic_template_name` + `generic_type_args` (provenance
  metadata para deduplicar especializaciones cross-module en AOT futuro).

Phase B desbloqueo D.2 (stackmaps + safepoints) y D.8 (C2 JIT con escape
analysis).

---

## 4. Phase C: libvesta_rt extraction (parcial)

**Status**: C.1+C.2 completos (2026-05-13); C.3 deferido.

- **C.1 (CERRADO)**: `include/vesta_rt/public.h` (~250 LOC) — API C estable con
  tipos opacos (`vrt_proc`, `vrt_vm`, `vrt_class`, `vrt_method`, `vrt_handle`)
  y wrappers para CADA runtime entry que el JIT/AOT debe invocar (GC alloc/
  deref/handle_for_ptr, monitors, exceptions, FFI, safepoint).
- **C.2 (CERRADO)**: `include/vesta_rt/abi.h` (~150 LOC) — constantes ABI
  explicitas para `ObjectHeader`, `StringObject`, codigos FatalError, conteo
  de regs VM. `static_assert(offsetof(...) == VESTA_*_OFFSET)` en
  `abi_checks.cpp` previene drift silencioso entre runtime y JIT/AOT.
- **C.3 (PENDIENTE)**: romper dependencias circulares `loader.h <-> runtime.h`
  para que `vesta_rt.lib` sea linkable contra consumers externos sin pull-in
  del frontend. Deferido hasta que AOT standalone lo necesite (Phase F).

---

## 5. Phase D: JIT C1+C2 (en progreso)

 **Status**: D.0-D.8 implementados (C1 eager + inline dispatch de CALLVIRT +
 OSR + profile counters + linear scan vreg + C2 optimizing con escape/ilp).
 El status anterior (2026-05-17) marcaba D.4-D.8 como pendientes; la rama
 `fork/fix/ci-lint-bug` los completó y ademas reparo bugs reales del JIT
 (deref de GcHandle en CALLVIRT, W^X del CodeCache, paso de argumentos a
 metodos JIT).  Cobertura del selector alta: todos los benches del repo
 eager-compilan main.  Pendiente real: D.9/D.10 parciales y el refactor AOT
 a `toolchain/` (en la rama feature de Desmon, no portado).

### Sub-hitos completados

- **D.0 (CERRADO 2026-05-13)**: Foundation. `jit::CodeCache` con VirtualAlloc/
  mmap PAGE_EXECUTE_READWRITE, `jit::RuntimeEntries` con tabla de wrappers
  `vrt_*` resueltos, bridge `enter_jit(fn, proc)` con convencion VM_ABI.
- **D.1 (CERRADO 2026-05-13)**: MachineIR + encoder x86-64 hand-rolled +
  selector minimal. ~2100 LOC. Decision: encoder propio (no Keystone) por
  10× mas rapido (~50 ns/instr vs ~10 us). MachineIR cache-friendly
  (`MInstr` exactamente 32 bytes, 2/cacheline).
- **D.2-foundation (CERRADO 2026-05-13)**: Safepoint flag en offset 0 de
  ProcessVM (disp0 encoding optimo, 4 bytes) + `vrt_safepoint_handler`
  skeleton + VM_ABI mode en selector + SAFEPOINT poll expansion en encoder.
- **D.2-int Fase 1 (CERRADO 2026-05-13)**: Stackmaps precisos additive con
  scan conservativo. Cero riesgo de use-after-free. ~800 LOC + 41 tests.
- **D.2-int Fase 2 (CERRADO 2026-05-13)**: Integracion de `scan_jit_roots_precise`
  en `gc_heap::major_gc`. Metricas `precise_roots_marked` /
  `precise_frames_scanned` / `conservative_roots_marked` en GcStats.
- **D.3-A (CERRADO 2026-05-13)**: PHI elimination + `JitCompiler` API +
  primer speedup medible **13.4× sobre interprete** en bench sum_to_n(100M).
- **D.3-B (CERRADO 2026-05-13)**: CALL support con resolver name ->
  RuntimeEntries + marshalling Native ABI + stackmap automatico en cada call.
- **D.3-C (CERRADO 2026-05-14)**: Auto-JIT trigger end-to-end. Opcion W: IR
  persistido en `.velb` v3. Hook `g_callvirt_post_hook` desacopla vesta_rt
  del JIT. Warning system toggleable (`VESTA_JIT_WARN_UNSUPPORTED=1`) para
  roadmap quirurgico.
- **D.3-D-G (CERRADO 2026-05-14)**: Cobertura del selector ampliada
  (conversiones int, raw_asm patterns frecuentes, CALLVIRT/CALLM/CALLCLOSURE/
  CALLN, runtime entries para class registry, mini-parser de raw_asm con
  symbol resolution). Coverage **47% -> 52%**.
- **D.3-H-I (CERRADO 2026-05-14)**: Symbol section (`VSYM`) + ABI offsets
  validados + CALLVIRT hot-path opt + INLINE CALLVIRT DISPATCH en JIT.

### Sub-hitos completados (desde el status 2026-05-17)

- **D.3-I+ (CERRADO)**: eager-compile de `main` funciona (verificado en todos
  los benches del repo).  Los callees no-JIT-ables (p.ej. `__module_init` con
  defclass/deffield) caen al interp via trampoline JIT->interp sin crashear.
- **D.4 (CERRADO)**: inline dispatch de CALLVIRT en el path vreg + inline
  cache interno de `vrt_callvirt_ic`.  `callvirt_hot` corre ~0.12s vs interp
  ~2.9s (~23x) en Release.
- **D.5 (CERRADO)**: OSR (On-Stack Replacement) con OSR-entries por loop y
  state-transfer C1->C2.  `tests/jit/test_osr.cpp` 8 checks.
- **D.6 (CERRADO)**: profile counters instrumentados (branch frequency, type
  observations en callvirt, alloc counts) con volcado `.vprof` al exit.
- **D.7 (CERRADO)**: linear scan register allocator target-aware en el
  pipeline vreg (build_intervals + linear_scan + rewrite_to_physical con
  stackmaps de GC roots).
- **D.8 (CERRADO)**: C2 optimizing con SSA passes (c2_compiler, c2_escape,
  c2_inliner, c2_licm, c2_deopt), regalloc vreg + linear scan.

### Sub-hitos pendientes (reales)
- **D.9 (CERRADO)**: PGO persistence en `.vprof` — implementado
  (`src/runtime/profile.cpp`, `--profile`/`VESTA_PROFILE_DUMP`).  El
  warm-start del JIT desde un `.vprof` previo queda como extension.
- **D.10 (PENDIENTE)**: AOT pipeline a `.velao` (sin recompile en cada
  startup).  El AOT actual genera ELF nativo (con malloc/free y relocaciones
  externas); el refactor completo a `toolchain/` (linker propio,
  multi-arquitectura) esta en la rama feature de Desmon, sin portar.

### Camino acelerado (solo JIT funcional, sin AOT/PGO completo)

~9 semanas desde estado actual hasta JIT que cubre la mayoria de programas
con C1 + IC + OSR. Speedup esperado: 2-3× sobre interp en codigo OOP-heavy,
hasta 20× en hot loops aritmeticos.

### Camino completo (C1+C2+AOT+PGO + nativo standalone)

~18-20 meses adicionales. Speedup esperado: 10-30× sobre interp.

---

## 6. Phase E: GC + safepoints + JIT integration (parcial)

**Status**: D.2 Fase 1+2 implementados (additive con conservativo).

Pendiente:
- **Fase 2-lean**: excluir rangos JIT del scan conservativo tras battle-test
  (~100+ tests e2e con JIT alocando). Elimina false positives.
- **Compaccion de OldGen**: requiere stackmaps precisos al 100% (sin
  conservativo). Permite moving GC en OldGen, mejor locality.
- **Concurrent GC** (largo plazo): mark/sweep en thread separado sin parar
  el programa.

---

## 7. Phase F: AOT + object emitter (pendiente)

**Status**: pendiente. Activos vendored: **LibPEparse** (~5934 LOC C) en
`libs/SourceCode/LibPEparse/` cubre parse + emit de PE/COFF/ELF. Examples
incluidos demuestran cada API.

Plan:

- **D.11 (~4 sem)**: C++ wrapper RAII (`include/jit/objfile.h`) sobre
  LibPEparse + completar piezas faltantes (cross-file reloc resolution,
  export tables, `.pdata`/`.eh_frame` sections para unwinding).
- **D.12 (~4 sem)**: Self-hosted linker. Parsea N `.obj`/`.o` files, construye
  global symbol table, resuelve relocs cross-file, genera imagen final via
  `PEBuilder` (Win) o `ELFBuilder` (Linux). Soporta linkear contra
  `vesta_rt.lib` como static lib.

Cuando esto se complete, `vm --aot programa.velb -o programa.exe` producira
un ejecutable nativo standalone sin necesidad de `vm.exe`.

---

## 8. Phase G: Native exception unwinding + debug (pendiente)

**Status**: pendiente.

- **D.13 (~5 sem)**: `throw` en codigo JIT/AOT unwinde sin volver al
  interprete.
  - Windows: `.pdata` + `.xdata` con UNWIND_INFO records. Usa
    `RtlLookupFunctionEntry`/`RtlUnwindEx`.
  - Linux/macOS: `.eh_frame` con DWARF unwind info + LSDA. Hand-rolled
    minimal unwinder (no libunwind para mantener cero deps).
- **D.14 (~4 sem)**: Native debug info para depurar `.exe` AOT con
  WinDbg/gdb/lldb.
  - Windows: CodeView (`.debug$S` con `S_GPROC32`, `S_LOCAL`, `S_LINE`).
  - POSIX: DWARF (`.debug_info`, `.debug_line`, `.debug_loc`, `.debug_str`).

---

## 9. Phase H: ARM/AArch64 + Linux target (pendiente)

**Status**: pendiente.

Tras x86-64 estable, anyadir backend ARM:

- Keystone ya soporta ARM/AArch64 (vendored).
- Cambio principal: instruction selector del D.1.b ARM-specific (~1500 LOC
  adicionales).
- Calling conventions: AAPCS64 (ARM64) vs SysV/Win64 (x86-64).
- Stackmaps: el formato es independiente del target, los offsets cambian.

ELF backend ya cubre Linux (D.11). macOS requiere variantes (Mach-O), pero
el plan para macOS comparte gran parte de la infraestructura ELF + DWARF.

---

## 10. Vision a largo plazo

Mas alla de Phase H, items que se han considerado pero no priorizado:

- **WASM backend**: emitir WebAssembly desde el SSA IR, ejecutar Vex en el
  browser. Requeriria portar `libvesta_rt` a WASM (GC stub via reference
  types).
- **GPU compute**: bytecode VM que ejecuta en GPU para workloads paralelos
  masivos. Requeriria SIMT abstraction sobre los procesos ligeros.
- **Lenguaje 2.0**: Vex post-Phase H podria incorporar features que hoy
  faltan: type classes / traits, effect system, dependent types.
- **Multi-lenguaje sobre IR**: el SSA IR + libvesta_rt podrian ser target
  de OTROS lenguajes (frontend Python-like, frontend Lisp, etc.). El proyecto
  esta ya estructurado para esto (B.3 monomorphization contract permite
  deduplicar across frontends).
- **Hot reload**: aprovechar `loadmodule` runtime para reemplazar codigo de
  un proceso vivo en runtime.

---

## Prioridades a corto plazo (proximos 3 meses)

Estado real (2026-08): los items 1-3 del plan original ya estan implementados
(eager main, inline dispatch CALLVIRT, OSR).  Las prioridades actuales son:

1. **C2 + unroll en programas con ILP real**: el C2 regalloc no da speedup en
   loops seriales (medido: regresion en array_sum/callvirt_hot); requiere
   validarse en programas con ILP donde el unroll del C2 exponga paralelismo.
2. **Refactor AOT a `toolchain/`**: el AOT actual emite ELF con malloc/free y
   relocaciones externas; el refactor completo de la rama feature de Desmon
   (linker propio, multi-arquitectura) no esta portado.
3. **Cobertura del selector D.3-J+**: cubrir mas IR ops hasta >80% de metodos
   reales JIT-compilables (hoy la mayoria de benches ya eager-compilan main).

---

## Como contribuir al roadmap

Si quieres trabajar en una fase pendiente:

1. Lee la seccion correspondiente en esta pagina y la doc relacionada en
   [doc/ARCHITECTURE.md](./ARCHITECTURE.md), [doc/VMdoc/IR/SSA.md](./VMdoc/IR/SSA.md),
   y los headers `include/jit/*.h` / `include/vesta_rt/*.h` (cada uno tiene
   Doxygen con la API y comentarios inline explicando decisiones de diseno).
2. Abre un issue en GitHub proponiendo el sub-hito (`Phase X.Y: ...`).
3. Coordina con el mainline para evitar duplicacion.
4. Ship con tests + docs actualizados.

Ver [CONTRIBUTING.md](./CONTRIBUTING.md) para el proceso completo.

---

## Phase Z: Memoria compartida cross-process (COMPLETA, 2026-05-23)

Phase Z se desarrollo en paralelo a Phases A-D para resolver la limitacion
arquitectural historica de t13: cada `ProcessVM` tenia su propio `gc::GcHeap`
privado, impidiendo `synchronized`/`notify`/`wait` cross-process.

**Status final**: COMPLETA (Z.1-Z.11 + Z.10 ext + Z.11 ext).

**Componentes principales** (~3580 LOC nuevos):

- `SharedHeap`: slab allocator lock-free con 12 size-classes y growth dinamico
  hasta ~1.5 GB total.
- `SharedHandleTable`: registry lock-free de hasta 4.19M handles con tagged-ABA.
- `WaitTable`: 4096 buckets con spinlock + backoff PAUSE/YIELD; split
  MONITOR/CONDVAR.
- `ObjectHeader.monitor_word` (ABI v3.1): atomic uint64 con encoded_pid 48-bit
  + lock_depth 16-bit -> exclusion mutua correcta cross-scheduler.
- Modificador `shared` en var-decl + builtins `share()`/`unshare()`/`is_shared()`.
- 8 opcodes nuevos (0xA6-0xAD): `newobjs`/`gcpromote`/`gcdemote` + atomic
  primitives + `sharedstat`.
- GC mark+sweep STW del SharedHeap coordinado multi-scheduler.

**Verificacion** (snapshot 2026-05-23):

- Vex e2e: **237/237 OK, 0 fallidos**.
- Tests Z unitarios: **8141 PASS**.
- `bench_shared_contention --schedulers 4`: R0 = 400000 exacto.
- t13 (Java-style synchronized + wait/notify cross-process): R0 = 42.

**Doc relacionada**:

- [doc/VMdoc/PhaseZ/SharedMemory.md](./VMdoc/PhaseZ/SharedMemory.md) - modelo completo.
- [doc/VMdoc/SetInstruccionesVM/PHASEZ_SHARED.md](./VMdoc/SetInstruccionesVM/PHASEZ_SHARED.md) - opcodes 0xA6-0xAD.
- [doc/VMdoc/SetInstruccionesVM/MONITOR.md](./VMdoc/SetInstruccionesVM/MONITOR.md) - ABI v3.1 del monitor_word.

---

Documentos relacionados:

- [doc/ARCHITECTURE.md](./ARCHITECTURE.md) - arquitectura interna actual
- [doc/BENCHMARKS.md](./BENCHMARKS.md) - performance numbers
- [doc/LANGUAGE.md](./LANGUAGE.md) - lo que Vex soporta hoy
- [doc/VMdoc/IR/SSA.md](./VMdoc/IR/SSA.md) - representacion intermedia + pasadas de optimizacion
- [doc/VMdoc/SetInstruccionesVM/](./VMdoc/SetInstruccionesVM/) - referencia del bytecode
- [doc/VMdoc/PhaseZ/SharedMemory.md](./VMdoc/PhaseZ/SharedMemory.md) - memoria compartida cross-process
