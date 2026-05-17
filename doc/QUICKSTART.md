# Quickstart - VestaVM en 5 minutos

Esta guia te lleva desde cero a tener VestaVM compilado, ejecutando programas
Vex, y abriendo el REPL interactivo. Si tienes problemas, consulta la seccion
final [Troubleshooting](#troubleshooting).

---

## Indice

- [Quickstart - VestaVM en 5 minutos](#quickstart---vestavm-en-5-minutos)
  - [Indice](#indice)
  - [1. Pre-requisitos](#1-pre-requisitos)
  - [2. Clonar y compilar](#2-clonar-y-compilar)
  - [3. Hola Mundo en Vex](#3-hola-mundo-en-vex)
  - [4. Ejecutar examples del repositorio](#4-ejecutar-examples-del-repositorio)
  - [5. REPL interactivo](#5-repl-interactivo)
  - [6. Probar el JIT](#6-probar-el-jit)
  - [7. Modo distribuido (2 nodos en localhost)](#7-modo-distribuido-2-nodos-en-localhost)
  - [8. Que probar a continuacion](#8-que-probar-a-continuacion)
  - [9. Troubleshooting](#9-troubleshooting)
    - ["config.guess: Syntax error: word unexpected" en Linux](#configguess-syntax-error-word-unexpected-en-linux)
    - ["CMake Error... at libs/SourceCode/keystone... policy version"](#cmake-error-at-libssourcecodekeystone-policy-version)
    - ["OPENSSL\_FOUND - NO" en Windows](#openssl_found---no-en-windows)
    - [El programa Vex no encuentra los plugins nativos (vesta\_io, etc.)](#el-programa-vex-no-encuentra-los-plugins-nativos-vesta_io-etc)
    - [Tests fallan](#tests-fallan)
    - [Build muy lento](#build-muy-lento)
    - [Run con Valgrind (Linux/macOS)](#run-con-valgrind-linuxmacos)

---

## 1. Pre-requisitos

| Plataforma | Toolchain                                  | OpenSSL                                            |
| :--------- | :----------------------------------------- | :------------------------------------------------- |
| **Linux**  | GCC >= 9 o Clang >= 10, CMake >= 3.10      | `sudo apt install libssl-dev` (Debian/Ubuntu)      |
| **Arch**   | `sudo pacman -S base-devel cmake`          | `sudo pacman -S openssl`                           |
| **macOS**  | Xcode CLI tools, `brew install cmake`      | `brew install openssl`                             |
| **Windows**| TDM-GCC-64 o MinGW-W64                     | [Win32/Win64 OpenSSL precompilado][openssl-win]    |

[openssl-win]: https://slproweb.com/products/Win32OpenSSL.html

CMake debe ser **>= 3.10**. Si tienes CMake 4.x, añadir
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` a los comandos del build para evitar
errores con los submodulos antiguos de Keystone.

---

## 2. Clonar y compilar

```bash
# Clonar el repo CON submodulos (Keystone, Capstone, LibPEparse vendored)
git clone --recursive https://github.com/desmonHak/VM.git
cd VM

# Build Release (recomendado para usar)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Build Debug (con asserts y simbolos para hackear el codigo)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
```

En Windows con MinGW:

```bash
cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

El binario resultante es `build/vm` (Linux/macOS) o `build/vm.exe` (Windows).
~15 MB en Release, estaticamente linkeado (sin DLL externas en Windows).

**Verificar el build**:

```bash
./build/vm --version
# -> VestaVM <version>

./build/vm --list-arch
# -> Lista de arquitecturas soportadas por Capstone/Keystone
```

---

## 3. Hola Mundo en Vex

Crea `hola.vex`:

```vex
i32 main() {
    println("Hola desde Vex ${1 + 1}!");
    return 0;
}
```

Compila y ejecuta:

```bash
./build/vm --vex hola.vex -o hola
./build/vm --run hola.velb
# -> Hola desde Vex 2!
```

**Que pasa**:

1. `vm --vex hola.vex -o hola` ejecuta el pipeline completo: lexer -> parser ->
   type checker -> lowering a SSA IR -> 15 pasadas de optimizacion -> emit
   `.vel` (ensamblador VM) -> assembler -> linker -> `hola.velb` (bytecode
   distribuible).

2. `vm --run hola.velb` carga el bytecode, mappea las secciones en memoria
   virtual de la VM, crea un ProcessVM, lo encola en el scheduler, y ejecuta
   `main()`.

---

## 4. Ejecutar examples del repositorio

El repo trae ~140 programas en [`examples_codes_vex/`](../examples_codes_vex/)
cubriendo todas las features del lenguaje:

```bash
# Factorial recursivo
./build/vm --vex examples_codes_vex/01_factorial.vex -o /tmp/fact
./build/vm --run /tmp/fact.velb
# -> 3628800

# Clases con herencia
./build/vm --vex examples_codes_vex/15_herencia_basica.vex -o /tmp/h
./build/vm --run /tmp/h.velb

# Async + futures
./build/vm --vex examples_codes_vex/43_async_basico.vex -o /tmp/a
./build/vm --run /tmp/a.velb
```

Para ver el `.vel` (ensamblador VM intermedio) o el SSA IR:

```bash
# Ver el .vel generado (lo escribe al lado del .velb)
./build/vm --vex examples_codes_vex/01_factorial.vex -o /tmp/fact
cat /tmp/fact.vel

# Volcar el SSA IR (pre y post optimizacion)
./build/vm --vex examples_codes_vex/01_factorial.vex -o /tmp/fact --vex-emit-ir
cat /tmp/fact.ir

# Generar diagramas Mermaid de AST, IR y .vel
./build/vm --vex examples_codes_vex/01_factorial.vex -o /tmp/fact --diagram-all
ls /tmp/fact.*.mmd
# -> /tmp/fact.ast.mmd, /tmp/fact.ir.pre.mmd, /tmp/fact.ir.post.mmd, /tmp/fact.vel.mmd
```

---

## 5. REPL interactivo

Ejecuta `vm` sin argumentos para abrir el REPL:

```bash
./build/vm
vesta [/path/to/cwd]>
```

Algunos comandos basicos:

```text
vesta> build src/main.vel -o programa            # compila
vesta> run miprog programa.velb                  # ejecuta en background con nombre
vesta> vms                                       # lista VMs activos
vesta> exec miprog 5 hello                       # pasa args a un VM en ejecucion
vesta> kill miprog                               # termina y libera el VM
vesta> alias ll="ls -la"                         # define alias
vesta> ll                                        # ejecuta el alias
```

Edicion de linea estilo readline:

- Flechas arriba/abajo: historial.
- `Ctrl+R`: busqueda incremental en historial.
- `TAB`: completion de comandos y rutas.
- `Ctrl+A`/`Ctrl+E`: inicio/fin de linea.
- `Ctrl+L`: limpia pantalla.

Referencia completa: [doc/CLI_REPL.md](./CLI_REPL.md) y
[doc/CLI_COMMANDS.md](./CLI_COMMANDS.md).

---

## 6. Probar el JIT

VestaVM tiene un **JIT C1** (template-based, sin regalloc) que da ~13x speedup
sobre el interprete en metodos hot. Se activa via `--jit-threshold N` o el
preset `-m jit` (= threshold 1, compila a la primera invocacion):

```bash
# Benchmark sin JIT (interprete puro)
./build/vm --run examples_codes_vex/benchmark/bench_jit_method.velb --stats
# -> Wall time: ~1700 ms, MIPS ~150

# Mismo bench con JIT activo
./build/vm --run examples_codes_vex/benchmark/bench_jit_method.velb -m jit --stats
# -> Wall time: ~85 ms, MIPS ~3500 (20x speedup)

# Stats de compilacion JIT
./build/vm --run examples_codes_vex/benchmark/bench_jit_method.velb -m jit --jit-stats
# -> Cuantos metodos compilados, cuantos fallaron (selector unsupported), etc.

# Warnings sobre que IR ops no soporta el selector
./build/vm --run programa.velb -m jit --jit-warn
```

Detalles: [doc/BENCHMARKS.md](./BENCHMARKS.md) y la seccion "JIT C1 baseline"
en [doc/ARCHITECTURE.md](./ARCHITECTURE.md).

---

## 7. Modo distribuido (2 nodos en localhost)

VestaVM puede crear clusters de nodos comunicandose via VDP (Vesta Distribution
Protocol) sobre TCP. Para una prueba rapida en localhost:

**Terminal 1** (nodo servidor):

```bash
./build/vm --dist-server --dist-port 7789 --dist-name nodo-1
# Queda esperando conexiones
```

**Terminal 2** (nodo cliente que envia un programa al nodo 1):

```bash
./build/vm --vex examples_codes_vex/47_rspawn_basico.vex -o /tmp/rsp
./build/vm --run /tmp/rsp.velb \
    --dist-port 7790 \
    --dist-add-node 127.0.0.1:7789
```

El programa hace `rspawn(0)` que envia un proceso al nodo 1, espera que se
ejecute y devuelva el resultado via Future.

Detalles completos: [doc/CLI_DIST.md](./CLI_DIST.md).

---

## 8. Que probar a continuacion

| Quieres...                              | Empieza por...                                       |
| :-------------------------------------- | :--------------------------------------------------- |
| Aprender el lenguaje Vex                | [doc/LANGUAGE.md](./LANGUAGE.md)                     |
| Ver ejemplos de cada feature            | [doc/EXAMPLES.md](./EXAMPLES.md)                     |
| Entender la arquitectura interna        | [doc/ARCHITECTURE.md](./ARCHITECTURE.md)             |
| Performance numbers + metodologia       | [doc/BENCHMARKS.md](./BENCHMARKS.md)                 |
| Roadmap completo (Phases A-H)           | [doc/ROADMAP.md](./ROADMAP.md)                       |
| Documentacion completa del lenguaje     | [doc/VMdoc/Vex/](./VMdoc/Vex/)                       |
| Reference del set de instrucciones      | [doc/VMdoc/SetInstruccionesVM/](./VMdoc/SetInstruccionesVM/) |

---

## 9. Troubleshooting

### "config.guess: Syntax error: word unexpected" en Linux

Los submodulos de Keystone tienen scripts con CRLF (line endings de Windows)
que algunos shells de Linux rechazan. Convertir a LF:

```bash
find libs/SourceCode -type f -exec dos2unix {} \;
```

Reintenta `cmake --build build`.

### "CMake Error... at libs/SourceCode/keystone... policy version"

Keystone fue escrito con CMake 2.x. Si tienes CMake 4.x, añadir:

```bash
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### "OPENSSL_FOUND - NO" en Windows

CMake no encuentra OpenSSL. Apuntale al directorio donde lo instalaste:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR="C:/OpenSSL-Win64" \
    -DOPENSSL_INCLUDE_DIR="C:/OpenSSL-Win64/include" \
    -DOPENSSL_LIBRARIES="C:/OpenSSL-Win64/lib"
```

### El programa Vex no encuentra los plugins nativos (vesta_io, etc.)

Los plugins se copian al directorio del binario `vm`. Si los movias o ejecutas
desde otro directorio, exporta `VESTA_PLUGIN_DIR`:

```bash
export VESTA_PLUGIN_DIR=/ruta/a/build/stdlib/native
./build/vm --run programa.velb
```

### Tests fallan

```bash
bash tests/vex/test_vex_e2e.sh build
# Esperado: 200/200 pasos OK
```

Si hay fallos, abrir issue con el output completo + version de
CMake/GCC/OpenSSL.

### Build muy lento

Activa paralelizacion:

```bash
cmake --build build -j$(nproc)        # Linux
cmake --build build -j$(sysctl -n hw.ncpu)  # macOS
cmake --build build -j%NUMBER_OF_PROCESSORS%  # Windows cmd
```

El primer build tarda ~3-5 minutos (compila Keystone + Capstone +
LibPEparse + VM). Builds incrementales: <30 segundos.

### Run con Valgrind (Linux/macOS)

```bash
cmake -B build-rwd -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rwd -j
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
    ./build-rwd/vm --run programa.velb
```

---

¿Algo no funciono? Abre un [issue en GitHub](https://github.com/desmonHak/VM/issues)
con tu plataforma, version de CMake/GCC, y el output completo del error.
