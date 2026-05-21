<div align="center">
  <img src="./Component 1.svg" width="180" height="180" alt="VestaVM logo" />

# VestaVM

**Una máquina virtual distribuida y un lenguaje moderno, diseñados juntos desde cero.**

[![Licencia](https://img.shields.io/badge/licencia-VMProject-blue.svg)](./LICENSE.md)
[![Estado](https://img.shields.io/badge/estado-Phase%20A%20Complete-brightgreen.svg)](./doc/ROADMAP.md)
[![Tests](https://img.shields.io/badge/tests-230%2F230%20PASS-brightgreen.svg)](./tests/vex/)
[![JIT](https://img.shields.io/badge/JIT-C1%20baseline%2020--60×-orange.svg)](./doc/BENCHMARKS.md)
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
y **Phase C/D parcial** (libvesta_rt + JIT C1 con cobertura ~52% de métodos
reales). **Phase MC** (macros lowered al IR + ejecutados via VM con cache
persistente y JIT opcional) también completa.

**Suite de tests E2E**: **230/230 PASS** (`tests/vex/test_vex_e2e.sh`), cubriendo
los 180+ ejemplos del repo + 6 tests negativos del borrow checker + 11 tests
positivos realistas del borrow checker.

**Performance: intérprete vs JIT** (best-of-3, hardware moderno, 2026-05-21):

| Benchmark | Intérp. wall | Intérp. MIPS | JIT wall | JIT efectivo | Speedup |
|---|---:|---:|---:|---:|---:|
| `bench_nested_loops` (tight nested) | 29 ms | 334 | 0.48 ms | ~20 GIPS | **61×** |
| `bench_alloc` (5M `new()` con GC) | 116 ms | 346 | 3.2 ms | ~12 GIPS | **36×** |
| `bench_polymorphic` (30M dispatch virtuales) | 683 ms | 302 | 22 ms | ~9.4 GIPS | **31×** |
| `bench_tight_loop` (50M iter ALU) | 1.14 s | 358 | 48 ms | ~8.5 GIPS | **24×** |
| `bench_jit_method` (30M iter en método) | 418 ms | 355 | 19 ms | ~7.8 GIPS | **22×** |
| `bench_callvirt_hot` (30M callvirt) | 316 ms | 345 | 15 ms | ~7.3 GIPS | **21×** |
| `bench_fib_recursive` (CALL/RET heavy) | 211 ms | 224 | 211 ms | 224 MIPS | 1.0× |

> **Nota sobre "JIT efectivo"**: el JIT emite código nativo x86-64 directo, no
> instrucciones VM. La columna mide cuántas instrucciones VM habrían sido
> necesarias para hacer el mismo trabajo en el intérprete, divididas por el
> wall time del JIT -- métrica comparable directamente con la MIPS del
> intérprete. El throughput x86 real es similar o superior (el JIT C1
> template emite ~3-5 instr x86 por instr VM original).

**Intérprete promedio**: ~340 MIPS sobre la suite completa (post sprint de
optimizaciones 2026-05-17: dispatcher threaded computed-goto, super-instrucciones,
regalloc coalescing, IR pass schedule, load_narrow elision).

**JIT C1**: hot methods con loops/aritmética/callvirt/alloc obtienen **20-60×**
sobre el intérprete. El único gap conocido es `bench_fib_recursive`
(CALL/RET-heavy) donde C1 no inlinea ni elide calls; C2 (Phase D.8 futuro)
lo resolverá. Cobertura del selector: ~52% de métodos reales JIT-compilados;
los no-soportados caen al intérprete graciosamente.

Detalles en [doc/BENCHMARKS.md](./doc/BENCHMARKS.md).

**Roadmap completo** (Phases B -> H, hasta AOT con ejecutables `.exe` nativos):
[doc/ROADMAP.md](./doc/ROADMAP.md).

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
| Ejecutables nativos | roadmap (Phase F) | externa (GraalVM) | ✓ | ✓ | externa | ✓ |
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
