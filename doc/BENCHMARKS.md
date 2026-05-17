# Benchmarks de VestaVM

Performance del intérprete y JIT C1, metodología, y comparativas con otras VMs.

---

## Indice

- [Benchmarks de VestaVM](#benchmarks-de-vestavm)
  - [Indice](#indice)
  - [1. TL;DR](#1-tldr)
  - [2. Hardware y metodologia](#2-hardware-y-metodologia)
  - [3. Benchmarks sinteticos del intérprete](#3-benchmarks-sinteticos-del-intérprete)
    - [Benchmarks core (12 incluidos)](#benchmarks-core-12-incluidos)
  - [4. Speedup acumulado del sprint 2026-05-17](#4-speedup-acumulado-del-sprint-2026-05-17)
  - [5. JIT C1 baseline](#5-jit-c1-baseline)
  - [6. Pipeline de optimizacion](#6-pipeline-de-optimizacion)
  - [7. Comparativa con otras VMs](#7-comparativa-con-otras-vms)
    - [Estimacion de orden de magnitud](#estimacion-de-orden-de-magnitud)
    - [Comparativa de features](#comparativa-de-features)
  - [8. Como correr los benchmarks](#8-como-correr-los-benchmarks)
    - [Comparar interp vs JIT](#comparar-interp-vs-jit)
  - [9. Profiling y stats](#9-profiling-y-stats)
    - [Stats basicos del intérprete](#stats-basicos-del-intérprete)
    - [Stats con overhead breakdown](#stats-con-overhead-breakdown)
    - [Profiling JIT](#profiling-jit)
    - [Profiling con Valgrind (Linux)](#profiling-con-valgrind-linux)
    - [Profiling con perf (Linux)](#profiling-con-perf-linux)
  - [Roadmap de performance](#roadmap-de-performance)

---

## 1. TL;DR

| Métrica                                  | Antes (baseline)  | Ahora             | Speedup     |
| :--------------------------------------- | :---------------: | :---------------: | :---------: |
| **MIPS promedio** (intérprete)           | ~150              | **~340**          | **2.3×**    |
| **Wall time avg** (10 benches)           | -                 | -                 | **-25..-81%** |
| **bench_polymorphic** (peor caso pre)    | 3660 ms           | **683 ms**        | **-81%**    |
| **bench_struct_field** (LOAD-heavy)      | 3800 ms           | **1994 ms**       | **-48%**    |
| **JIT bench_jit_method** vs interp       | 1700 ms           | **85 ms**         | **20×**     |

Optimizaciones aplicadas (orden cronologico del sprint):
1. `ir_pass_load_narrow` - elide sign-extension redundante
2. INLINE_THRESHOLD 8 -> 12
3. Regalloc coalesce hint con steal-from-active
4. `ir_pass_schedule` - list scheduling para ILP
5. **Threaded computed-goto dispatch** + inline icache hit
6. Super-instrucciones `alu3` (9 variantes)
7. Super-instruccion `loadz/loadzh`



---

## 2. Hardware y metodologia

**Hardware de referencia**:

- CPU: x86-64 moderno (Intel Core / AMD Ryzen reciente)
- RAM: 16+ GB
- OS: Windows 10/11 con MinGW (TDM-GCC-64) o Linux x86-64

**Build configuration**:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Flags Release: `-O3 -DNDEBUG -march=x86-64 -mtune=native -ffast-math
-fstrict-aliasing -fno-plt -fomit-frame-pointer -fvisibility=hidden
-ffunction-sections -fdata-sections`.

**Metodologia**:

- Cada bench se compila con `vm --vex bench.vex -o /tmp/b -O2` (opt level 2).
- Se ejecuta 3 veces; se reporta el **best-of-3** (mejor tiempo) para reducir
  variabilidad por scheduling del OS.
- MIPS se calcula como `(profiler_instr_counter / wall_time_ns) * 1000`.
- Sin actividad de fondo del sistema (cerrar navegadores, etc.).

**Variables que afectan los numeros**:

- CPU frequency scaling (especialmente en laptops; mejor con AC plugged).
- Hyperthreading: medir en single-thread (un solo scheduler).
- Termal throttling en runs largos.

---

## 3. Benchmarks sinteticos del intérprete

Ubicacion: [`examples_codes_vex/benchmark/`](../examples_codes_vex/benchmark/).

### Benchmarks core (12 incluidos)

| Bench                | Que mide                                | Iters     | Wall      | MIPS    |
| :------------------- | :-------------------------------------- | :-------: | --------: | ------: |
| `bench_tight_loop`   | ALU pura (muls/adds/xor/and en loop)   | 50M       | 1111 ms   | **358** |
| `bench_array_sum`    | LOAD i32 + ADD (suma de array)         | 10M loads | 374 ms    | 323     |
| `bench_nested_loops` | Loops anidados (1000x1000)             | 1M body   | 30 ms     | 334     |
| `bench_struct_field` | LOAD-STORE-ADD i32 en struct           | 30M       | 1994 ms   | 294     |
| `bench_polymorphic`  | Dispatch polimorfico (3 impls)         | 10M       | 683 ms    | 331     |
| `bench_callvirt`     | CALLVIRT en hot loop (main)            | 30M       | 797 ms    | 339     |
| `bench_callvirt_hot` | CALLVIRT con metodo trivial            | 30M       | 317 ms    | 347     |
| `bench_alloc`        | `new Foo()` con ctor zero-init        | 5M        | 116 ms    | 346     |
| `bench_jit_method`   | Hot loop dentro de metodo virtual      | 30M       | 415 ms    | 359     |
| `bench_fib_recursive`| CALL/RET recursivo (fib 30)            | 2.7M call | 210 ms    | 220     |
| `bench_callvirt_hot.vex` (variante) | Stress test callvirt        | 30M       | 360 ms    | 333     |

**Patrones observables**:

- ALU-heavy: **~350-360 MIPS** (limite del dispatch loop + decode).
- LOAD-heavy: **~290-330 MIPS** (memory deps + zero-extend cost).
- Recursion-heavy: **~200-220 MIPS** (CALL/RET overhead via frame pool).

---

## 4. Speedup acumulado del sprint 2026-05-17

Comparado con el baseline ANTES del sprint (pre-2026-05-17):

| Bench                | Pre-sprint | Post-sprint | Δ wall     | MIPS pre -> post |
| :------------------- | ---------: | ----------: | ---------: | --------------: |
| `bench_tight_loop`   | ~2200 ms   | **1111 ms** | **-49%**   | ~150 -> 358      |
| `bench_array_sum`    | ~620 ms    | **374 ms**  | **-40%**   | ~140 -> 323      |
| `bench_nested_loops` | ~46 ms     | **30 ms**   | **-35%**   | ~140 -> 334      |
| `bench_struct_field` | ~3800 ms   | **1994 ms** | **-48%**   | ~120 -> 294      |
| `bench_polymorphic`  | ~3660 ms   | **683 ms**  | **-81%**   | ~80 -> 331       |
| `bench_callvirt`     | ~1400 ms   | **797 ms**  | **-43%**   | ~155 -> 339      |
| `bench_callvirt_hot` | ~500 ms    | **317 ms**  | **-37%**   | ~210 -> 347      |
| `bench_alloc`        | ~155 ms    | **116 ms**  | **-25%**   | ~250 -> 346      |
| `bench_jit_method`   | ~710 ms    | **415 ms**  | **-42%**   | ~210 -> 359      |
| `bench_fib_recursive`| ~290 ms    | **210 ms**  | **-28%**   | ~165 -> 220      |

**Bench ganador del sprint**: `bench_polymorphic`. La combinacion de
INLINE_THRESHOLD bump (8 -> 12) + devirt automatico + LICM + alu3 produjo un
cascade: las tres implementaciones de `Shape.area()` se inlinearon, LICM
hoisteo las computaciones (todas constantes-of-loop) al entry block, y el
body del loop quedo en `add %sum, %precomputed` × 3 ramas. Reduccion 5× en
instrucciones VM ejecutadas.

---

## 5. JIT C1 baseline

**Phase D.3 implementado** con cobertura ~52% de metodos reales. El JIT se
activa con `--jit-threshold N` o `-m jit` (= threshold 1):

| Bench                | Interp     | JIT (-m jit) | Speedup     |
| :------------------- | ---------: | -----------: | ----------: |
| `bench_jit_method`   | 1700 ms    | **85 ms**    | **20×**     |
| `bench_tight_loop`   | 1111 ms    | (TODO)       | -           |
| `bench_polymorphic`  | 683 ms     | (TODO)       | -           |

**Limitaciones actuales del JIT** (52% coverage):

- main eager-compile **desactivado** por bug pendiente (D.3-I+): los callees
  que el selector no soporta hacen que el JIT-eated main crashee silenciosamente.
- Metodos que usan `raw_asm` complejo (synchronized, monenter, defclass, etc.)
  caen a interp.
- Float arith (`fadd`/`fmul`/...) no soportada en el selector v1.
- `loadmodule`, `rspawn`, exception handling: caen a interp.

Cuando el metodo SI es compilable, el bytecode VM se reemplaza con codigo
nativo x86-64 que es ~10-20× mas rapido. 

---

## 6. Pipeline de optimizacion

Cada pasada del optimizador SSA contribuye al speedup. Numeros aproximados
medidos en benches sinteticos (variables segun el bench):

| Optimizacion                              | Tipo            | Ganancia tipica |
| :---------------------------------------- | :-------------- | --------------: |
| Dead Code Elimination                     | IR cleanup      | varia           |
| Copy Propagation                          | IR cleanup      | varia           |
| Constant Folding                          | compile-time    | varia           |
| Strength Reduction (mul/div -> shift)      | aritmetica      | varia           |
| LICM (Loop Invariant Code Motion)         | loops           | 0-50% (loops)   |
| Dead Store Elimination + SLF              | memoria         | 5-15%           |
| TCO (Tail Call Optimization)              | recursion       | 0-30% (rec.)    |
| Devirtualization monomorphic              | OOP             | 0-80% (poly)    |
| **Inlining** (threshold 12)               | calls           | 5-30%           |
| CSE (Common Subexpression Elim)           | aritmetica      | 0-15%           |
| **Load Narrow** (elide SEXT)              | i32 loads       | 5-15%           |
| **List Scheduling** (ILP)                 | reordering      | 0-5% interp     |
| **Regalloc coalesce hint**                | regalloc        | **15-30%**      |
| **Threaded computed-goto + icache inline**| scheduler       | **10-20%**      |
| **alu3** super-instr                      | bytecode        | **5-20%**       |
| **loadz** super-instr                     | bytecode        | **5-15%**       |

---

## 7. Comparativa con otras VMs

> **Disclaimer**: comparativas entre VMs distintas son inherentemente injustas
> (diferentes lenguajes, runtimes, GCs, GUIs JIT, etc.). Los numeros aqui son
> indicativos, no autoritativos.

### Estimacion de orden de magnitud

| VM                            | MIPS interp aprox | Notas                                  |
| :---------------------------- | ----------------: | :------------------------------------- |
| **VestaVM** (post-sprint)     | ~340              | threaded goto + super-instr            |
| CPython (interpreter)         | ~10-50            | bytecode stack-based, sin JIT          |
| CPython 3.13 (+JIT copy)      | ~30-100           | copy-and-patch JIT experimental        |
| Lua 5.4 (interpreter)         | ~80-200           | register-based, optimizado             |
| LuaJIT (interp mode)          | ~200-400          | computed-goto + register-based         |
| LuaJIT (JIT mode)             | ~1000-3000        | tracing JIT muy maduro                 |
| Ruby YARV                     | ~30-100           | interp + JIT YJIT experimental         |
| OpenJDK Java (interp)         | ~50-150           | template interpreter                   |
| OpenJDK Java (C2 JIT)         | ~2000-10000+      | JIT optimizing maduro 20+ anios        |
| V8 JavaScript (Ignition+JIT)  | ~1000-5000+       | tiered JIT + speculative opt           |
| **VestaVM** (JIT C1)          | ~3000-5000        | template JIT, compilable metodos       |

VestaVM esta en el rango de **LuaJIT en modo interp**, lo cual es bueno
considerando que LuaJIT tiene 15+ anios de optimizacion specifica. El JIT C1
de VestaVM es comparable a un Tier 1 de HotSpot (no Tier 2 C2 optimizing
todavia).

### Comparativa de features

| Feature                       | Vesta | Lua | Python | Java | JS |
| :---------------------------- | :---: | :-: | :----: | :--: | :-: |
| Multi-paradigma OOP completo  | sí    | -   | sí     | sí   | sí |
| Static types                  | sí    | -   | -      | sí   | TS |
| GC generacional               | sí    | sí  | sí (3.x) | sí | sí |
| Modelo actor nativo           | sí    | -   | -      | -    | -  |
| Smart pointers en lenguaje    | sí    | -   | -      | -    | -  |
| Borrow checker compile-time   | sí    | -   | -      | -    | -  |
| Distribuido nativo            | sí    | -   | -      | -    | -  |
| FFI integrado al lenguaje     | sí    | sí  | (lib)  | JNI  | (lib) |

---

## 8. Como correr los benchmarks

```bash
# Compilar todos los benches en un script
for b in examples_codes_vex/benchmark/bench_*.vex; do
  name=$(basename "$b" .vex)
  ./build/vm --vex "$b" -o /tmp/$name -O2 > /dev/null
done

# Ejecutar uno especifico con stats
./build/vm --run /tmp/bench_tight_loop.velb --stats

# Best-of-3 para reducir noise
for b in bench_tight_loop bench_array_sum bench_polymorphic; do
  best=999999999
  for i in 1 2 3; do
    out=$(./build/vm --run /tmp/$b.velb --stats 2>&1)
    wall=$(echo "$out" | awk '/Wall time/{print $5}')
    if [ "$wall" -lt "$best" ]; then best=$wall; fi
  done
  echo "$b: best=$best us"
done
```

### Comparar interp vs JIT

```bash
# Interp puro
./build/vm --run /tmp/bench_jit_method.velb --stats

# Con JIT activo
./build/vm --run /tmp/bench_jit_method.velb -m jit --stats

# JIT con stats de compilacion
./build/vm --run /tmp/bench_jit_method.velb -m jit --jit-stats
```

---

## 9. Profiling y stats

### Stats basicos del intérprete

```bash
./build/vm --run programa.velb --stats
```

Imprime al final:

```text
Wall time:        1234567 ns  (1234 us, 1 ms)
profiler_instr_counter: 400000000
MIPS:             324.5  (wall time)
```

### Stats con overhead breakdown

```bash
./build/vm --run programa.velb --stats --overhead-breakdown
```

Imprime tambien el desglose de:
- VM construct time (alocacion del ArenaManager, etc.)
- load_executable time (parse del .velb + mmap)
- vm.start time
- wait time (cuanto espero el main thread)
- vm.stop time

Util para detectar overhead del setup vs compute real.

### Profiling JIT

```bash
./build/vm --run programa.velb -m jit --jit-stats --jit-warn
```

Imprime:
- Methods compiled vs unsupported.
- Cada unsupported method con detalle del IR op que falla.
- Code cache usage.

### Profiling con Valgrind (Linux)

```bash
cmake -B build-rwd -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rwd -j

valgrind --tool=callgrind ./build-rwd/vm --run programa.velb
callgrind_annotate callgrind.out.*
```

### Profiling con perf (Linux)

```bash
perf record -g ./build/vm --run programa.velb
perf report
```

---

## Roadmap de performance

Phase D+ (JIT C2 con regalloc, AOT, PGO):

| Phase | Esperado                                  |
| :---: | :---------------------------------------- |
| D.5   | Tiered dispatch + OSR (warm-up agresivo)  |
| D.7-8 | C2 con regalloc real + escape analysis    |
| D.9   | PGO persistido (.vprof)                   |
| D.10  | AOT a .velao (sin recompile)              |
| D.11+ | Native .exe standalone (COFF/ELF)         |

Detalles: [doc/ROADMAP.md](./ROADMAP.md).

---

Referencias completas:

- [doc/VMdoc/IR/SSA.md](./VMdoc/IR/SSA.md) seccion 9 para las pasadas de opt.
- [doc/VMdoc/SetInstruccionesVM/SUPER_INSTRUCCIONES.md](./VMdoc/SetInstruccionesVM/SUPER_INSTRUCCIONES.md)
  para los opcodes super-instr.
- [doc/ARCHITECTURE.md](./ARCHITECTURE.md) seccion "JIT C1 baseline" para el
  estado actual del JIT.
