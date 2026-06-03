<div align="center">
  <img src="./Component 1.svg" width="180" height="180" alt="VestaVM logo" />

# VestaVM

**Una máquina virtual distribuida y un lenguaje moderno, diseñados juntos desde cero.**

[![Licencia](https://img.shields.io/badge/licencia-VMProject-blue.svg)](./LICENSE.md)
[![Estado](https://img.shields.io/badge/estado-Phase%20A%20Complete-brightgreen.svg)](./doc/ROADMAP.md)
[![Tests](https://img.shields.io/badge/tests-213%2F213%20PASS-brightgreen.svg)](./tests/vex/)
[![JIT](https://img.shields.io/badge/JIT-C1%20geomean%2016×%20%2F%20peak%20155×-orange.svg)](./doc/BENCHMARKS.md)
[![Plataformas](https://img.shields.io/badge/plataformas-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#inicio-r%C3%A1pido)

[**Inicio rápido**](./doc/QUICKSTART.md) · [**El lenguaje Vex**](./doc/LANGUAGE.md) · [**Arquitectura**](./doc/ARCHITECTURE.md) · [**Benchmarks**](./doc/BENCHMARKS.md) · [**Roadmap**](./doc/ROADMAP.md)

</div>

---

## ¿Qué es VestaVM?

**VestaVM** es una plataforma completa de ejecución de programas que integra tres
componentes diseñados desde cero para trabajar juntos:

1. **[Vex](./doc/LANGUAGE.md)** — un lenguaje multi-paradigma estáticamente tipado
   con sintaxis C/Java/Python combinada. Soporta clases con herencia/interfaces/AOP,
   genéricos, async/await, pattern matching, borrow checker estilo Rust, smart
   pointers (`unique<T>`/`shared<T>`), FFI nativo declarativo, reflexión runtime,
   y un sistema de **metaprogramación** compile-time potente (`@Macro`, captura
   raw `expr`, introspección zero-overhead, FFI en tiempo de compilación).

2. **Compilador y runtime** — pipeline `Vex -> SSA IR -> bytecode .velb -> VM`. Más
   de 15 pasadas de optimización SSA, dispatcher threaded computed-goto
   (~340 MIPS sostenidos), GC generacional con stack scanning conservativo +
   stackmaps precisos, y JIT C1 template-based con **20-60× speedup** sobre
   el intérprete en métodos hot (throughput efectivo ~7-20 GIPS de instr-VM
   equivalentes).

3. **Sistema distribuido nativo (VDP)** — protocolo propio sobre TCP/TLS para
   `rspawn` (spawn remoto), mensajería entre nodos, descubrimiento UDP en LAN,
   y autenticación token/mTLS. La distribución es parte del bytecode, no RPC
   externo.

El proyecto incluye también una shell interactiva (REPL), un lenguaje de scripting
embebido (VestaShellScript `.vsh`), un debugger TCP, y un editor TUI escrito
en el propio Vex.

---

## Ejemplo: cómo se ve Vex

```vex
// Pattern matching, genéricos, smart pointers y borrow checker en 20 líneas.

enum Result<V, E> {
    Ok(V),
    Err(E)
}

unique<HashMap> safe_divide(i32[] nums, i32 divisor) -> Result<unique<HashMap>, string> {
    if (divisor == 0) return Err("division by zero");
    
    unique<HashMap> result = unique_box(hashmap(16));
    borrow_mut<HashMap> view = lend_mut(result);
    
    for (i32 n in nums) {
        write_borrow(view).put(n, n / divisor);
    }
    return Ok(move(result));
}

i32 main() {
    i32[] data = {10, 20, 30, 40};
    match safe_divide(data, 2) {
        case Ok(map)  => println("Got ${map.size()} entries");
        case Err(msg) => println("Error: ${msg}");
    }
    return 0;
}
```

Más ejemplos completos en [`examples_codes_vex/`](./examples_codes_vex/) (180+
programas) y showcase curado en [doc/EXAMPLES.md](./doc/EXAMPLES.md).

---

## Características destacadas

### Lenguaje (Vex)

- **Multi-paradigma**: imperativo + POO + funcional ligero. Sin envoltura
  `class` obligatoria.
- **Tipado estático con inferencia local** y nullability explícita
  (`Optional<T>`, `Result<V,E>`, `nonnull T`, `T !!name`).
- **POO completa**: clases, herencia simple + interfaces, propiedades get/set,
  modificadores `public`/`private`/`protected`/`static`/`final`, destructores
  RAII.
- **AOP nativo**: `@Aspect` con `@Before`/`@After`/`@Around` y `proceed()`.
- **Reflexión runtime**: `forName`, `getClass`, `getField`, `getMethod`,
  `invoke`, `newInstance`.
- **Genéricos** por monomorphización compile-time + fallback runtime con
  `specialize`.
- **Async / concurrencia**: `@Async`/`await`/`Future<T>`, `spawn`/`spawn here`/
  `spawn on(N)`/`rspawn`, mailboxes (`msgsend`/`msgrecv`), `synchronized` con
  cleanup automático.
- **Pattern matching** exhaustivo (`match`/`case` con bindings).
- **Smart pointers** zero-overhead: `unique<T>`, `shared<T>` con deleters custom
  para adoptar cualquier recurso del SO.
- **Borrow checker** compile-time estilo Rust con 4 reglas + NLL + reborrow con
  suspend stack + lifetime elision.
- **FFI** declarativo a DLLs (`extern "lib.dll" { fn ...; }`) y runtime
  (`ffi_open`/`ffi_sym`/`ffi_call`).
- **Strings** UTF-8/16/32, interpolación `${expr}`, format specifiers
  `${expr:hex:>20}`, triple-quoted.
- **Metaprogramación compile-time**: `@Macro` que genera código Vex inyectable,
  captura raw de expresiones arbitrarias (`asm walk(ptr -> 0x10 -> 0x20)`),
  introspección de tipos sin overhead (`sizeof<T>`, `typename<T>`, `kind<T>`,
  `field_count<T>`, `for_each_field<T>`), FFI en tiempo de compilación que
  invoca DLLs del sistema durante el build y embebe los resultados como
  literales, `static_assert(cond, "msg")` con condiciones comptime-evaluables.

### Runtime y compilador

- **Pipeline SSA completo**: ~15 pasadas (DCE, CSE, copy-prop, const-fold, TCO,
  LICM, DSE+SLF, devirt+inline, load_narrow, list scheduling para ILP).
- **Asignador de registros linear scan** con register hinting / coalesce
  (steal-from-active).
- **Dispatcher threaded computed-goto** + inline del icache hit path
  (intérprete ~340 MIPS promedio).
- **JIT C1** baseline (template-based, sin regalloc real) con stackmaps
  precisos para GC. Throughput efectivo **~7-20 GIPS** (instr-VM equivalentes
  por segundo), **20-60× speedup** sobre intérprete en métodos hot.
- **Super-instrucciones**: `cmpjmp`/`cmpjmpu`, `decjnz`, `alu3` (9 variantes
  fusionando `mov+OP`), `loadz`/`loadzh` (zero-extend LOAD), `mvtake`,
  `gcallocp`, `spawnargs`, `fulfillhlt`.
- **GC generacional** Young/Old con stack scanning conservativo + interior
  pointer scanning + write barriers para colecciones externas.
- **Multi-threading** real opcional con scheduler placement (`spawn here`,
  `spawn on(N)`).
- **Profile-Guided Optimization (PGO) foundation**: contadores runtime
  zero-overhead (~1 ciclo cuando inactivo, atomic load + branch
  predicted-not-taken) instrumentan branches condicionales (taken/not_taken
  por PC), tipos observados en call sites virtuales (`callvirt`/`callm`,
  hasta 4 tipos distintos por site + contador de megamorfismo polimórfico),
  y allocations por PC. Volcado a fichero binario `.vprof` al exit del
  proceso, consumible por el JIT (warm-start de funciones hot del run
  anterior) y por el compilador AOT (decisiones especulativas hard-coded
  en el ejecutable nativo).

### Sistema distribuido

- **Protocolo VDP** nativo sobre TCP, opcionalmente cifrado con TLS (mTLS o
  token CRAM SHA-256).
- **Descubrimiento UDP** automático de nodos en LAN.
- **rspawn** transparente: el bytecode envía procesos a nodos remotos como si
  fueran locales; `Future<T>` resuelve cross-node automáticamente.
- **memsync** para sincronización de regiones de memoria entre nodos.

### Herramientas

- **REPL** con TAB completion, historial, búsqueda incremental Ctrl+R, aliases,
  variables de entorno, scripts de inicio (`~/.vestarc`).
- **VestaShellScript (.vsh)** — lenguaje embebido para scripting del REPL.
- **Debugger TCP** con protocolo JSON: breakpoints (por addr o `file.vex:line`),
  step/continue, inspección de registros/memoria/stack, GC stats, source-aware.
- **Diagramas Mermaid** del pipeline: `--diagram-vex/ir/vel/all` para AST, SSA
  IR, bytecode visualizado.
- **Map file de simbolos** opt-in via `--emit-map` (debug; off por defecto
  porque cuesta ~60% del tiempo del linker).
- **Profiler del linker** integrado via `VESTA_LINKER_PROFILE=1` (timing
  fase a fase + bytes emitidos por sección).
- **Ensamblador/desensamblador nativo** integrado (Keystone + Capstone) para
  x86, x86_64, ARM, AArch64.
- **Profile dump** via `--profile [path]` (default: `program.vprof`).
  Alternativa por entorno: `VESTA_PROFILE_DUMP=path`. Genera `.vprof`
  binario al exit con counters de branches, tipos observados en cada
  call site polimórfico, y allocations por PC. Sirve tanto al JIT
  (warm-start) como al pipeline AOT futuro (PGO en ejecutables nativos).

---

## Inicio rápido

```bash
# Clonar (incluye submódulos: Keystone, Capstone, LibPEparse)
git clone --recursive https://github.com/desmonHak/VM.git
cd VM

# Compilar (CMake + GCC/Clang)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Hola Mundo en Vex
cat > hola.vex << 'EOF'
i32 main() {
    println("Hola desde Vex ${1 + 1}!");
    return 0;
}
EOF

./build/vm --vex hola.vex -o hola
./build/vm --run hola.velb
# -> Hola desde Vex 2!
```

Guía completa: [**doc/QUICKSTART.md**](./doc/QUICKSTART.md) (5 minutos).

---

## Estado del proyecto

VestaVM está en **Phase A completa** (frontend Vex + intérprete + GC + distribuido)
y **Phase C/D parcial** (libvesta_rt + JIT C1 con cobertura **~87%** de métodos
reales). **Phase MC** (macros lowered al IR + ejecutados via VM con cache
persistente y JIT opcional) también completa. **PGO foundation** disponible
vía `--profile <path>` que genera `.vprof` consumible por el JIT (warm-start)
y el futuro compilador AOT (decisiones especulativas hard-coded).

**Suite de tests E2E**: **213/213 PASS** (`tests/vex/test_vex_e2e.sh`), cubriendo
los 180+ ejemplos del repo + 6 tests negativos del borrow checker + 11 tests
positivos realistas del borrow checker.

### Estadísticas clave

Datos generados por `tools/bench/run_all_benches.py` sobre **27 workloads
multi-lenguaje** en hardware Intel Core i7-13700KF (16P/24L, 3.4 GHz
base) + 47.8 GB RAM, Windows 10, 3 runs + 1 warmup por bench, AV
desactivado durante el bench, 27/27 benches completados sin timeouts:

| Métrica | Valor |
|---|:---:|
| **Suite e2e Vex** | 213/213 PASS (0 FAIL) |
| **Intérprete: MIPS promedio** | ~340 (threaded computed-goto + super-instr) |
| **JIT C1: cobertura del selector** | **~87%** de métodos reales |
| **JIT vs intérprete: speedup geomean** | **18.95×** sobre 27 benchmarks |
| **JIT vs intérprete: speedup median** | **15.5×** |
| **JIT vs intérprete: peak speedup** | **184.6×** (`intops_jit`) |
| **JIT vs intérprete: ≥100×** | 2/27 benches |
| **JIT vs intérprete: ≥50×** | 7/27 benches |
| **JIT vs intérprete: ≥25×** | 11/27 benches |
| **JIT vs intérprete: ≥10×** | 19/27 benches (70%) |
| **JIT geomean slowdown vs C nativo** | **10.42×** |
| **HotSpot C2 geomean slowdown vs C** | 11.49× |
| **C++ geomean slowdown vs C** | 1.01× (paridad) |
| **Vex JIT vs HotSpot (geomean global)** | **JIT ~9-10% más rápido que HotSpot** |
| **JIT vs HotSpot: vence (>10%) en** | **15/27** benches (56%) |
| **JIT vs HotSpot: paridad ±10%** | 6/27 benches (22%) |
| **JIT vs HotSpot: Java vence (>10%)** | 6/27 benches (22%) |
| **JIT vs CPython 3.11: supera en** | 25/27 benches (93%) |

**Top speedups intérprete → JIT** (mediana de 3 runs):

| Benchmark | Intérp. | JIT | Speedup |
|---|---:|---:|---:|
| `intops_jit` (int ops mixtas) | 6 710 ms | **36 ms** | **185×** |
| `rotops_jit` (rotaciones bitwise) | 4 246 ms | **36 ms** | **117×** |
| `hash_lookup` (FNV-style 50M iter) | 8 583 ms | **104 ms** | **83×** |
| `mem_malloc_free` (5M malloc+free) | 2 478 ms | **40 ms** | **62×** |
| `int_mixed` (10 int ops/iter × 20M) | 4 994 ms | **81 ms** | **62×** |
| `state_machine` (FSM 10M tokens) | 4 366 ms | **80 ms** | **54×** |
| `bitops` (popcnt/clz/ctz heavy) | 3 994 ms | **78 ms** | **52×** |
| `memcpy_loop` (1MB × 100 iter) | 2 470 ms | **71 ms** | **35×** |
| `array_sum` (10M loads + ADD) | 1 477 ms | **56 ms** | **26×** |
| `nested_loops` (1000×1000) | 2 118 ms | **81 ms** | **26×** |
| `tight_loop` (50M iter ALU) | 2 048 ms | **81 ms** | **25×** |
| `fib_recursive` (recursión profunda) | 618 ms | **45 ms** | **14×** |

**Intérprete promedio**: ~340 MIPS sobre la suite completa. Dispatcher
threaded computed-goto, super-instrucciones (alu3, loadz, cmpjmp, decjnz),
regalloc coalescing, IR pass schedule, load_narrow elision.

**JIT C1**: cobertura del selector **~87%** de métodos reales (subió desde
63% inicial). Hot loops aritméticos consiguen **50-155× speedup**. La
recursión profunda — caso histórico difícil para JITs C1 — pasa de
1.0× a **13× speedup** gracias a TAILCALL emitido nativamente
(`call + ret` fusionados sin FrameHeader pool) + self-recursive call
patcheado directo a `code_start` (sin trampoline JIT→intérprete). Los
no-soportados restantes (excepciones polimórficas, spawn/distrib,
futures) caen al intérprete graciosamente.

### Comparativa multi-lenguaje (workloads idénticos)

Tiempos wall en ms (mediana de 3 runs; hardware i7-13700KF). Para cada
fila, el **valor más rápido marcado en negrita**:

| Bench | C | C++ | Vex JIT | Java HotSpot 25 | Python 3.11 |
|---|---:|---:|---:|---:|---:|
| `alloc` | 3.0 | **2.7** | 33.4 | 69.8 | 535.6 |
| `array_sum` | 4.7 | **4.6** | 55.9 | 77.5 | 423.7 |
| `bitops` | **25.8** | 25.9 | 77.5 | 90.0 | 7169.7 |
| `branch_unpredict` | 17.3 | **17.1** | 562.7 | 165.0 | 3016.0 |
| `callvirt` | **2.6** | 2.9 | 54.5 | 68.0 | 1771.8 |
| `callvirt_hot` | 10.5 | **4.6** | 45.3 | 68.5 | 691.9 |
| `cmp_fusion` | **2.8** | **2.8** | 65.7 | 65.9 | 1812.9 |
| `fib_recursive` | **5.8** | 11.2 | 43.7 | 74.9 | 206.8 |
| `fp_jit` | **9.8** | **9.8** | 93.5 | 84.6 | 1316.7 |
| `hash_lookup` | **12.4** | 13.4 | 103.7 | 128.3 | 6523.5 |
| `int_mixed` | **18.4** | **18.4** | 80.6 | 83.0 | 10736.0 |
| `intops_jit` | **3.0** | 5.7 | 36.3 | 71.6 | 1064.7 |
| `jit_method` | **3.7** | 3.8 | 49.0 | 76.8 | 1165.0 |
| `mem_class` | **2.7** | 3.6 | 83.1 | 72.1 | 147.7 |
| `mem_malloc_free` | 5.2 | **4.2** | 39.7 | 70.1 | 556.3 |
| `mem_struct` | **2.8** | 3.3 | 129.3 | 68.7 | 394.8 |
| `memcpy_loop` | 7.3 | **6.3** | 71.2 | 99.3 | 3053.5 |
| `nested_loops` | **13.6** | 14.1 | 80.6 | 85.1 | 1572.5 |
| `pic_real` | **5.2** | 5.9 | 485.5 | 71.3 | 274.4 |
| `polymorphic` | 10.8 | **8.8** | 55.9 | 81.5 | 1000.9 |
| `quicksort` | 9.9 | **7.1** | 40.3 | 74.2 | 116.0 |
| `rotops_jit` | **3.6** | 4.8 | 36.4 | 78.8 | 1360.9 |
| `state_machine` | **19.4** | **19.4** | 80.2 | 84.9 | 1544.6 |
| `string_hot` | 8.0 | 5.1 | 30.0 | 84.9 | **25.0** |
| `string_workout` | **30.2** | 39.8 | 416.9 | 192.5 | 524.4 |
| `struct_field` | 5.2 | **5.1** | 80.1 | 74.3 | 5152.1 |
| `tight_loop` | **12.4** | **12.4** | 80.8 | 75.6 | 992.4 |

**Vex JIT C1 vence a HotSpot C2 en 15 de 27 benches (56%)**, paridad
±10% en 6 (22%), Java vence claramente en 6 (22%). En geomean global
de slowdown vs C, **Vex JIT está en 10.42× mientras HotSpot está en
11.49×** — VestaVM es ~9-10% más rápido que Java en promedio.

Casos donde JIT queda detrás (target de optimizaciones futuras):

- `pic_real` (JIT 485 ms vs Java 71 ms) — el JIT C1 no devirtualiza
  el patrón PIC con clases dispersas; el inliner futuro lo cerrará.
- `branch_unpredict` (563 ms vs 165 ms) — branches genuinamente
  impredecibles; mejora con branch hints del perfil PGO ya recolectado.
- `string_workout` (417 ms vs 192 ms) — HotSpot tiene
  small-string-optimization; mejora con SSO en StringObject.
- `mem_struct` (129 ms vs 69 ms) — overhead de struct copy en JIT.

**Vex JIT supera a CPython 3.11 en 25 de 27 benches (93%)**. Las dos
excepciones son `string_hot` (CPython tiene refcount + SSO nativos) y
`mem_class` (Python pequeño data + sin GC overhead aquí). El peor caso
de Vex sigue siendo significativamente mejor que el mejor caso de
Python en hot loops puros.

Detalles + metodología + cómo correr los benchmarks en
[doc/BENCHMARKS.md](./doc/BENCHMARKS.md). Los workloads multi-lenguaje
viven en `examples_codes_vex/benchmark/<bench>/` con `main.vex`, `main.c`,
`main.cpp`, `main.py`, `Main.java` cada uno. Runner orquestador:
`python tools/bench/run_all_benches.py` (genera `bench_results.json`
con `runs_individual`, `stats` p50/p95/min/max/stddev por lenguaje, y
9 gráficas matplotlib en `bench_plots/` + reporte HTML navegable).

### Visualización gráfica de resultados

El runner genera un dashboard completo en `bench_plots/index.html` con
9 vistas distintas. Las más representativas:

**Dashboard global** (`01_dashboard.png`): cuatro paneles con wall time
absoluto por lenguaje, comparativa vs C nativo, ranking por bench y
speedup JIT vs intérprete:

![Dashboard global](./bench_plots/01_dashboard.png)

**Resumen geomean vs C** (`08_geomean_summary.png`): media geométrica
del slowdown vs C nativo por lenguaje. Vex JIT compite directamente
con HotSpot C2 y los supera ligeramente en promedio:

![Geomean vs C](./bench_plots/08_geomean_summary.png)

**Heatmap completo** (`02_heatmap.png`): tiempo wall por bench × lenguaje,
colores verde-amarillo-rojo según velocidad relativa:

![Heatmap](./bench_plots/02_heatmap.png)

**Ratio vs C nativo bench-by-bench** (`07_grouped_ratio.png`): ratio
de slowdown contra C para cada bench, agrupado por lenguaje:

![Ratio vs C](./bench_plots/07_grouped_ratio.png)

**Variabilidad de mediciones** (`04_boxplot_variability.png`): boxplots
de min/p50/p95/max por lenguaje × bench. Vex JIT tiene la varianza más
baja entre las VMs (interp también), demostrando determinismo del
dispatcher:

![Variabilidad](./bench_plots/04_boxplot_variability.png)

Vistas adicionales disponibles en `bench_plots/`:
[`00_system_info.png`](./bench_plots/00_system_info.png) (hardware +
toolchains usados),
[`03_radar_profile.png`](./bench_plots/03_radar_profile.png) (perfil
radar por lenguaje),
[`05_scatter_ratio_vs_c.png`](./bench_plots/05_scatter_ratio_vs_c.png)
(scatter de ratio vs C), y
[`06_ranking_lines.png`](./bench_plots/06_ranking_lines.png) (ranking
por bench mostrando consistencia entre lenguajes).  El runner además
genera una gráfica dedicada por cada uno de los 27 benches en
`bench_plots/per_bench/` (no commiteadas, regenerar localmente con
`python tools/bench/run_all_benches.py`).

**Roadmap completo** (hasta JIT C2 con regalloc real + AOT con ejecutables
`.exe` nativos en 3 tiers): [doc/ROADMAP.md](./doc/ROADMAP.md).

---

## Comparativa de features

VestaVM no busca competir con runtimes maduros en velocidad bruta o ecosistema,
sino ofrecer un **ecosistema completo y autocontenido** donde lenguaje, runtime,
distribución y herramientas se diseñan juntos. Comparativa de features clave:

| Feature | VestaVM (Vex) | Java/JVM | Rust | C++ | Python | Go |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| Tipado estático con inferencia local | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ |
| Pattern matching exhaustivo | ✓ | parcial (21+) | ✓ | parcial (C++26) | parcial | ✗ |
| Borrow checker compile-time | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ |
| Smart pointers con deleters custom | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ |
| Reflexión runtime | ✓ | ✓ | parcial | ✗ | ✓ | ✓ |
| AOP nativo (`@Aspect`/proceed) | ✓ | externa (AspectJ) | ✗ | ✗ | externa | ✗ |
| Genéricos por monomorphización | ✓ | ✗ (erasure) | ✓ | ✓ | ✗ | ✓ |
| Async/await + futures | ✓ | ✓ | ✓ | parcial | ✓ | goroutines |
| Spawn distribuido transparente | ✓ | externa (Akka) | externa | ✗ | externa | ✗ |
| Metaprogramación compile-time | ✓ | ✗ | macros | templates | metaclasses | ✗ |
| FFI compile-time (DLLs en build) | ✓ | ✗ | macros build.rs | ✗ | ✗ | ✗ |
| `static_assert` con condiciones runtime-evaluables | ✓ | ✗ | const fn | ✓ | ✗ | ✗ |
| Captura raw de DSL embebido | ✓ (`expr`) | ✗ | `macro_rules!` | macros texto | ✗ | ✗ |
| Format specs en interpolación | ✓ | ✗ | ✓ | parcial (C++20) | ✓ | ✓ |
| Strings UTF-8/16/32 nativos | ✓ | UTF-16 | UTF-8 | varios | varios | UTF-8 |
| GC generacional precise+conservative | ✓ | ✓ | ✗ | ✗ | refcount+gc | ✓ |
| JIT integrado | ✓ (C1) | ✓ (C1+C2+Graal) | ✗ | ✗ | parcial (PyPy) | ✗ |
| Bytecode portable | ✓ (`.velb`) | ✓ (.class) | ✗ | ✗ | ✓ (.pyc) | ✗ |
| Ejecutables nativos (3 tiers: con runtime / embebido / sin runtime) | roadmap (Phase AOT) | parcial (GraalVM) | ✓ (no_std) | ✓ (freestanding) | externa | ✓ |
| Profile-Guided Optimization (PGO) | ✓ (foundation `.vprof`) | ✓ | externa (cargo-pgo) | ✓ (GCC/MSVC) | externa | parcial |
| Inline assembly | roadmap (Phase AS) | ✗ | ✓ | ✓ | ✗ | parcial |
| REPL interactivo | ✓ | ✓ (JShell) | ✗ | ✗ | ✓ | ✗ |
| Debugger source-aware integrado | ✓ (TCP) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Diagramas Mermaid del pipeline | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ |
| Zero deps externas | ✓ | JVM | crates | varios | CPython | runtime Go |

### Donde brilla VestaVM

- **Ecosistema autocontenido**: lenguaje + runtime + JIT + distribución + REPL
  + debugger + visualización en un solo binario ~15 MB sin runtime externo.
- **Distribución como ciudadano de primera**: `rspawn(node) { ... }` + `await`
  cross-node es parte del bytecode, no una librería bolt-on.
- **Metaprogramación end-to-end**: macros con captura raw de DSLs, introspección
  zero-overhead, FFI en tiempo de compilación, todo en el mismo lenguaje.
- **Seguridad de memoria moderna**: borrow checker estilo Rust + smart pointers
  + GC para los casos en los que el borrow checker es demasiado restrictivo.
- **Hand-rolled donde importa**: encoder x86-64 propio para el JIT (10× más
  rápido que Keystone), GC propio, scheduler propio, protocolo distribuido
  propio. Sin "impedance mismatch" entre capas.

### Donde NO brilla (todavía)

- **Madurez del ecosistema**: 0 paquetes públicos vs millones en npm/cargo/maven.
- **Velocidad bruta**: el JIT C1 está al 5-10% de la velocidad de C nativo
  optimizado en hot loops (C2 cerrará la brecha cuando llegue).
- **Documentación en inglés**: toda la doc del proyecto está en español ASCII.
- **Plataformas ARM/AArch64**: el JIT solo soporta x86-64 hoy; ARM en Phase H.
- **Madurez del Phase D**: cobertura del JIT C1 al ~52% de métodos reales;
  el resto cae al intérprete (que es rápido pero no nativo).

---

## Documentación

### Para empezar

| Documento | Para qué sirve |
|---|---|
| [QUICKSTART](./doc/QUICKSTART.md) | Instalación + primer programa en 5 minutos |
| [LANGUAGE](./doc/LANGUAGE.md) | Visión general del lenguaje Vex |
| [EXAMPLES](./doc/EXAMPLES.md) | Catálogo curado de ejemplos por tema |
| [ARCHITECTURE](./doc/ARCHITECTURE.md) | Arquitectura interna de la VM |
| [BENCHMARKS](./doc/BENCHMARKS.md) | Performance comparada + metodología |
| [ROADMAP](./doc/ROADMAP.md) | Plan de fases A-H, estado actual |

### Referencia del lenguaje (doc/VMdoc/Vex/)

| Sintaxis y semántica | Modelo de programación |
|---|---|
| [TiposDatos](./doc/VMdoc/Vex/TiposDatos.md) | [OOP](./doc/VMdoc/Vex/OOP.md) |
| [Operadores](./doc/VMdoc/Vex/Operadores.md) | [Generics](./doc/VMdoc/Vex/Generics.md) |
| [ControlFlow](./doc/VMdoc/Vex/ControlFlow.md) | [ReflexionAOP](./doc/VMdoc/Vex/ReflexionAOP.md) |
| [Strings](./doc/VMdoc/Vex/Strings.md) | [Metaprogramacion](./doc/VMdoc/Vex/Metaprogramacion.md) |
| [OptionalResult](./doc/VMdoc/Vex/OptionalResult.md) | [Colecciones](./doc/VMdoc/Vex/Colecciones.md) |
| [Closures](./doc/VMdoc/Vex/Closures.md) | [Excepciones](./doc/VMdoc/Vex/Excepciones.md) |

| Memoria y seguridad | Concurrencia y FFI |
|---|---|
| [SmartPointers](./doc/VMdoc/Vex/SmartPointers.md) | [Async](./doc/VMdoc/Vex/Async.md) |
| [BorrowChecker](./doc/VMdoc/Vex/BorrowChecker.md) | [Sincronizacion](./doc/VMdoc/Vex/Sincronizacion.md) |
| | [FFI](./doc/VMdoc/Vex/FFI.md) |

### Referencia de la VM (doc/VMdoc/)

- [SSA IR](./doc/VMdoc/IR/SSA.md) — formato intermedio + ~15 pasadas de optimización
- [SetInstruccionesVM/](./doc/VMdoc/SetInstruccionesVM/) — 50+ docs por familia
  de opcodes bytecode
- [SUPER_INSTRUCCIONES](./doc/VMdoc/SetInstruccionesVM/SUPER_INSTRUCCIONES.md) —
  opcodes fusionados (alu3, loadz/loadzh, cmpjmp, decjnz, mvtake, etc.)
- [runtime/](./doc/VMdoc/runtime/) — ProcessVM, scheduler, GC, plugins nativos
- [Generics](./doc/VMdoc/Generics/Generics.md), [Debug](./doc/VMdoc/Debug/),
  [Hilos](./doc/VMdoc/Hilos/), [Distribuido](./doc/VMdoc/Distribuido/)

### Herramientas y CLI

- [CLI_REPL](./doc/CLI_REPL.md) — REPL interactivo (edición de línea, historial,
  aliases, scripts)
- [CLI_COMMANDS](./doc/CLI_COMMANDS.md) — referencia completa de comandos
- [CLI_DIST](./doc/CLI_DIST.md) — runtime distribuido desde el REPL
- [CLI_VSH](./doc/CLI_VSH.md) — VestaShellScript (.vsh)

### Proyecto

- [LICENSE](./LICENSE.md) — licencia VMProject
- [CONTRIBUTING](./doc/CONTRIBUTING.md) — cómo contribuir
- [DEPENDENCIES](./doc/DEPENDENCIES.md) — librerías necesarias
- [github_work](./doc/github_work.md) — GitFlow del proyecto

---

## Construir desde código fuente

### Dependencias

| Plataforma | Comando |
|---|---|
| Linux (apt) | `sudo apt install build-essential cmake libssl-dev` |
| Arch Linux | `sudo pacman -S base-devel cmake openssl` |
| macOS | `brew install cmake openssl` |
| Windows | TDM-GCC-64 o MinGW + [precompiled OpenSSL](https://slproweb.com/products/Win32OpenSSL.html) |

Submódulos vendored (Keystone, Capstone, LibPEparse) se clonan automáticamente
con `--recursive`. Cero deps externas adicionales.

### Build

```bash
# Release (recomendado)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Debug (con símbolos + asserts)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Windows MinGW
cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Si la versión de CMake instalada es ≥ 4.x y falla por las versiones mínimas de
Keystone, añadir `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

Detalles completos (Valgrind, ASan, errores comunes, XMake alternativo) en
[doc/QUICKSTART.md](./doc/QUICKSTART.md).

---

## Casos de uso

- **Investigación**: prototipado rápido de runtimes, optimizaciones IR,
  esquemas de GC, JITs y sistemas distribuidos sin pelearse con LLVM.
- **Servicios distribuidos**: ejecutar workloads que cruzan nodos LAN con
  `rspawn` + `Future<T>` sin RPC manual.
- **Aplicaciones embebidas**: la VM compila a un único binario estático ~15 MB,
  sin dependencias dinámicas en Release.
- **DSLs y generación de código**: el sistema de macros `@Macro` + `expr`
  capture permite implementar mini-lenguajes embebidos (parsers, builders,
  pattern matching custom) cuyo código se reduce a literales en compile-time.
- **Aprendizaje de lenguajes**: el código del frontend Vex (~30K LOC) está
  diseñado para ser legible, con cada feature documentada y comentada
  exhaustivamente en español ASCII.

### Casos de uso futuros (Phase AOT)

La arquitectura está preparada para tres tiers de deployment cuando aterrice
la compilación a binarios nativos. La **misma fuente** podrá compilar a:

- **Vex Full** (3-5 MB con runtime linkado dinámicamente) — apps managed
  con todo el lenguaje disponible: GC, async, reflexión, distribución.
  Modelo análogo a Go, Java, C#.
- **Vex Embed** (500 KB – 1 MB con mini-runtime embebido estáticamente) —
  CLI tools, ETL, scripts standalone. Subset del lenguaje sin reflexión
  ni distribución pero con clases/strings/closures/excepciones. Modelo
  análogo a Rust con std, Swift, OCaml native.
- **Vex Bare** (50-200 KB sin runtime, solo libc o freestanding) —
  desarrollo de sistemas operativos, drivers de kernel, firmware
  embedded ARM Cortex-M/RISC-V, bootloaders y aplicaciones UEFI,
  hot-path libs distribuidas como `.dll`/`.so` con cero overhead vs
  C++ optimizado. Modelo análogo a Zig minimal, Rust `#![no_std]`,
  C/C++ embedded.

Vex Bare incluirá mecanismos de extensibilidad para que el programador
implemente lo que falta según su caso de uso: `@AllocatorOverride` para
hookear `kmalloc/kfree` del kernel, `@PanicHandler` para reemplazar el
default `fputs+exit` por halt CPU / reboot / log a UART, `@CustomGC`
para implementaciones especializadas (refcount, region-based, Boehm
conservativo), `@StringImpl` para UTF-8 ligero sin GC, `@SyncImpl` para
spinlocks / IRQ-disable / futex según nivel, `@UnwindImpl` para
`setjmp/longjmp` como alternativa a `.pdata/.eh_frame`. Detalle completo
en el plan AOT del roadmap.

---

## Filosofía del proyecto

VestaVM no busca competir con JVM/CLR/Cranelift en madurez ni con C/Rust en
performance bruta. Su valor está en ser un **ecosistema completo y autocontenido**
donde lenguaje, runtime, distribución y herramientas se diseñan juntos:

- **Decisiones cohesionadas**: el GC sabe del JIT, el JIT sabe del bytecode, el
  bytecode sabe del lenguaje. No hay "impedance mismatch".
- **Hand-rolled cuando importa**: encoder x86-64 propio (10× más rápido que
  Keystone para JIT), object emitter PE/COFF/ELF integrado, linker propio.
  Sin dependencias externas en el path crítico.
- **Documentación binding**: cada feature del lenguaje y cada opcode del
  bytecode tienen doc autoritativa en español. 
  referencia para futuras decisiones).
- **Tests no negociables**: 230/230 e2e antes de cada commit. Cada nueva feature
  ships con su test en `tests/vex/`.

---

## Comunidad

- **Repositorio principal**: [github.com/desmonHak/VM](https://github.com/desmonHak/VM)
- **Documentación Obsidian-friendly**: [github.com/desmonHak/VMdoc](https://github.com/desmonHak/VMdoc)
- **Issues y feature requests**: [GitHub Issues](https://github.com/desmonHak/VM/issues)
- **Cómo contribuir**: [doc/CONTRIBUTING.md](./doc/CONTRIBUTING.md)

---

## Licencia

VestaVM se distribuye bajo la **licencia VMProject**. Lee
[LICENSE.md](./LICENSE.md) para los términos completos.

Submódulos con licencias propias:
- **Keystone** y **Capstone**: BSD 3-Clause.
- **LibPEparse**: ver `libs/SourceCode/LibPEparse/LICENSE`.
- **OpenSSL**: Apache 2.0 / OpenSSL License (según versión).
- **nlohmann/json**: MIT.
- **cxxopts**: MIT.
- **FTXUI**: MIT.

---

<div align="center">

**VestaVM** · una máquina virtual diseñada como ecosistema.

Hecho con C++17, GCC y mucho café por [desmonHak](https://github.com/desmonHak).

</div>
