<div align="center">
  <img src="./Component 1.svg" width="180" height="180" alt="VestaVM logo" />

# VestaVM

**Una máquina virtual distribuida y un lenguaje moderno, diseñados juntos desde cero.**

[![Licencia](https://img.shields.io/badge/licencia-VMProject-blue.svg)](./LICENSE.md)
[![Estado](https://img.shields.io/badge/estado-Phase%20A%20Complete-brightgreen.svg)](./doc/ROADMAP.md)
[![Tests](https://img.shields.io/badge/tests-200%2F200%20PASS-brightgreen.svg)](./tests/vex/)
[![JIT](https://img.shields.io/badge/JIT-C1%20baseline%2013×-orange.svg)](./doc/BENCHMARKS.md)
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
   pointers (`unique<T>`/`shared<T>`), FFI nativo declarativo, y reflexión runtime.

2. **Compilador y runtime** — pipeline `Vex -> SSA IR -> bytecode .velb -> VM`. Más
   de 15 pasadas de optimización SSA, dispatcher threaded computed-goto, GC
   generacional con stack scanning conservativo + stackmaps precisos, y JIT C1
   template-based que da ~13× speedup sobre el intérprete.

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

Más ejemplos completos en [`examples_codes_vex/`](./examples_codes_vex/) (~140
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

### Runtime y compilador

- **Pipeline SSA completo**: ~15 pasadas (DCE, CSE, copy-prop, const-fold, TCO,
  LICM, DSE+SLF, devirt+inline, load_narrow, list scheduling para ILP).
- **Asignador de registros linear scan** con register hinting / coalesce
  (steal-from-active).
- **Dispatcher threaded computed-goto** + inline del icache hit path
  (intérprete ~340 MIPS promedio).
- **JIT C1** baseline (template-based, sin regalloc) con stackmaps precisos
  para GC. Speedup ~13× sobre intérprete en métodos hot.
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
reales).

**Suite de tests E2E**: **200/200 PASS** (`tests/vex/test_vex_e2e.sh`), cubriendo
los 140+ ejemplos del repo + 6 tests negativos del borrow checker.

**Performance del intérprete** (best-of-3, hardware moderno):

| Benchmark | Wall time | MIPS |
|---|---:|---:|
| `bench_tight_loop` (50M iter ALU) | 1.11 s | 358 |
| `bench_array_sum` (10M loads i32) | 374 ms | 323 |
| `bench_polymorphic` (3 impls × 10M dispatch) | 683 ms | 331 |
| `bench_callvirt_hot` (30M callvirt) | 317 ms | 347 |
| `bench_jit_method` (30M iter en método) | 415 ms | 359 |

Speedup acumulado tras el sprint de optimizaciones 2026-05-17: **-25% a -81%
wall time**, MIPS de ~150 a ~340 promedio. Detalles en
[doc/BENCHMARKS.md](./doc/BENCHMARKS.md).

**Roadmap completo** (Phases B -> H, hasta AOT con ejecutables `.exe` nativos):
[doc/ROADMAP.md](./doc/ROADMAP.md).

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
| [Strings](./doc/VMdoc/Vex/Strings.md) | [Colecciones](./doc/VMdoc/Vex/Colecciones.md) |
| [OptionalResult](./doc/VMdoc/Vex/OptionalResult.md) | [Excepciones](./doc/VMdoc/Vex/Excepciones.md) |
| [Closures](./doc/VMdoc/Vex/Closures.md) | |

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
- **Tests no negociables**: 200/200 e2e antes de cada commit. Cada nueva feature
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
