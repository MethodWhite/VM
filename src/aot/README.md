# Vex Bare AOT (Ahead-of-Time) Compiler

Compilador AOT para VestaVM que produce ejecutables nativos directamente desde
la SSA IR, sin necesidad del runtime de la VM en el destino.

## Tres Tiers

| Tier   | Runtime                    | GC | Scheduler | Async | Distribucion | Tamano tipico |
|--------|----------------------------|----|-----------|-------|--------------|---------------|
| Full   | Completo (vrt_*)           | Si | Si        | Si    | Si           | 3-5 MB        |
| Embed  | Minimo (sin GC/dist)       | No | No        | Basico| No           | 500 KB-1 MB   |
| Bare   | Freestanding (sin runtime) | No | No        | No    | No           | 50-200 KB     |

### Full

Incluye el runtime completo de VestaVM: garbage collector (GC tracing
generational), scheduler cooperativo, async/await, futures, distribucion
de mensajes entre nodos, monitores, y manejo de excepciones.

Ideal para aplicaciones de usuario que usan el ecosistema Vex completo.

### Embed

Mini-runtime sin GC ni distribucion.  Mantiene soporte basico de
async/await y manejo de excepciones.  Los objetos se alocan con
malloc/free directamente (sin tracing).  No hay recolector de basura.

Adecuado para dispositivos embebidos con recursos limitados donde el
overhead del GC no es aceptable.

### Bare

Sin runtime alguno.  Codigo freestanding puro, ejecutable directamente
en bare metal, kernels, o entornos embedded profundos.  No hay malloc,
no hay excepciones, no hay hilos.  El unico soporte es el que
proporciona el codigo usuario.

Las funciones marcadas como `@native` se resuelven como simbolos
externos que deben ser linkados manualmente.

## Build

### Requisitos

- CMake 3.20+
- Compilador C++17 (GCC 9+, Clang 10+, MSVC 2019+)
- Para ejecutables ELF: linker GNU ld o mold (opcional)
- Para ejecutables PE: linker MSVC o MinGW

### Integracion en CMake

El AOT compiler se compila como parte de la libreria `vex_lib`.
Anadir en `CMakeLists.txt`:

```cmake
target_sources(vex_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/src/aot/aot_compiler.cpp
    ${CMAKE_SOURCE_DIR}/src/aot/elf_emitter.cpp
)
```

### Compilacion manual

```bash
# Compilar objeto .o
g++ -std=c++17 -I include -c src/aot/aot_compiler.cpp -o aot_compiler.o
g++ -std=c++17 -I include -c src/aot/elf_emitter.cpp -o elf_emitter.o

# Linkear con el runtime segun tier
# FULL:
g++ aot_compiler.o elf_emitter.o -lvesta_rt -o vex_aot

# EMBED:
g++ aot_compiler.o elf_emitter.o -o vex_aot_embed

# BARE (freestanding):
g++ -ffreestanding -nostdlib aot_compiler.o elf_emitter.o -o vex_aot_bare
```

## Uso

```cpp
#include "aot/aot_compiler.h"
#include "ir/ssa_ir.h"

// 1. Parsear o construir modulo IR
ir::IrModule mod;
// ... llenar modulo con funciones ...

// 2. Crear compilador AOT
aot::AotCompiler compiler;
compiler.set_tier(aot::Tier::BARE);
compiler.set_output_format(aot::OutputFormat::ELF);

// 3. Compilar
aot::AotResult result = compiler.compile(mod);

if (result.ok) {
    // Escribir ejecutable
    FILE *f = fopen("output.elf", "wb");
    fwrite(result.executable.data(), 1, result.executable.size(), f);
    fclose();

    printf("Compilado: %zu bytes de codigo, %zu bytes de datos\n",
           result.code_size, result.data_size);
}
```

## Ejemplo: Hola Mundo Bare

Dado un programa Vex:

```vex
@module hello

@function main() -> i32 {
entry:
    %0 = const.i32 42
    ret.i32 %0
}
```

Compilar con Bare AOT produce un ejecutable ELF64 de ~120 bytes que:

1. Arranca en `_start`
2. Llama a `main`
3. Toma el return value y llama `exit_group(42)` via syscall
4. No depende de libc ni runtime VM

```bash
./vex_aot hello.ir -o hello --tier bare --format elf
chmod +x hello
./hello
echo $?  # imprime 42
```

## Pipeline Interno

```
IrModule
  |
  v
ir::ir_optimize()       -- optimizacion SSA (DCE, const-fold, CSE, etc.)
  |
  v
ir::compute_liveness()  -- analisis de intervalos de vida
  |
  v
ir::allocate_regs()     -- asignacion de registros (linear scan)
  |
  v
jit::Selector           -- seleccion de instrucciones (IrFunction -> MFunction)
  |
  v
jit::X86Encoder         -- emision de bytes x86-64
  |
  v
ElfEmitter              -- generacion de archivo objeto ELF64 / ejecutable
  |
  v
AotResult               -- bytes + metadatos
```

## Formatos de Salida

### ELF (Linux)

| Componente | Descripcion |
|------------|-------------|
| ELF Header | e_ident, ET_REL/ET_EXEC, EM_X86_64 |
| .text      | Codigo maquina x86-64 |
| .rodata    | Datos estaticos inmutables (string literals, constantes) |
| .data      | Datos mutables con valor inicial |
| .bss       | Datos zero-inicializados |
| .symtab    | Tabla de simbolos (funciones, globales) |
| .strtab    | Nombres de simbolos |
| .shstrtab  | Nombres de secciones |
| .rela.text | Relocaciones para enlazado |
| .eh_frame  | Informacion de excepciones (FULL/EMBED) |
| .comment   | Version del compilador |

En modo ejecutable (ET_EXEC) se generan program headers:
- PT_LOAD para .text (R+X), .rodata (R), .data+.bss (R+W)
- PT_GNU_STACK (R+W+X)
- PT_GNU_RELRO

### PE (Windows) - Planeado

Soporte para formato PE32+ con secciones .text, .data, .rdata, .reloc.
El emisor PE se implementara en una fase posterior.

### Mach-O (macOS) - Planeado

Soporte para formato Mach-O 64-bit con segmentos __TEXT, __DATA, __LINKEDIT.
Se requiere implementacion del emitter Mach-O.

## Limitaciones

1. **Solo x86-64**: El emisor de codigo nativo usa el encoder del JIT
   existente que solo soporta x86-64.  ARM64 y RISC-V requieren nuevos
   encoders.

2. **ELF-only v1**: Solo el formato ELF64 esta completamente implementado.
   PE y Mach-O son placeholders para desarrollo futuro.

3. **Sin linker externo**: En modo Bare, el ejecutable se genera directamente
   sin pasar por ld.  Esto limita las relocaciones complejas (TLS, shared
   libraries).  Para FULL/EMBED se recomienda enlazar con ld.

4. **Sin PIE**: Los ejecutables generados no son Position-Independent.
   Se cargan en una direccion fija (0x400000).

5. **Runtime minimo**: El tier Embed usa una implementacion minima de
   async que puede no ser compatible con todas las APIs de VestaVM.

6. **Sin debug info**: No se generan secciones .debug_* (DWARF).
   La depuracion requiere compilar con el JIT/interprete.

## Trabajo Futuro

- [ ] Emisor PE (Windows)
- [ ] Emisor Mach-O (macOS)
- [ ] Soporte ARM64 (AArch64)
- [ ] Enlazador interno completo (relocaciones, resolucion de simbolos)
- [ ] Generacion de DWARF para depuracion
- [ ] PIE (Position-Independent Executables)
- [ ] Soporte thread-local storage (TLS)
- [ ] LTO (Link-Time Optimization) integrado
- [ ] Perfilado PGO guiado por el JIT
- [ ] Compilacion incremental (cache de objetos .o)
- [ ] Soporte para shared libraries (.so/.dll/.dylib)
- [ ] Cross-compilation (target != host)
