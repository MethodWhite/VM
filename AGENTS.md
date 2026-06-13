# VestaVM — AI Agent Guide

## Build Instructions

```bash
# Debug build (development)
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DVESTA_BUILD_STDLIB=ON -DVESTA_BUILD_EXAMPLES=ON
cmake --build build --parallel

# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DVESTA_BUILD_STDLIB=ON -DVESTA_BUILD_EXAMPLES=ON
cmake --build build --parallel
```

## Code Style Guidelines

- C++17 standard, C11 for C code
- 4-space indentation, no tabs
- `snake_case` for functions and variables, `PascalCase` for types/classes
- Header guards: `#pragma once`
- Keep functions small and focused
- Avoid unnecessary dependencies
- Complex code must be documented in Spanish or English
- No premature optimizations

## Project Structure

```
├── src/                  # Core VM source
│   ├── runtime/          # Execution engine, scheduler, GC
│   ├── vex/              # Vex language frontend (lexer, parser, type-checker)
│   ├── jit/              # JIT compiler
│   ├── ir/               # SSA IR
│   ├── loader/           # Bytecode loader, class registry
│   ├── arena/            # Virtual memory, TLB, arena manager
│   ├── gc/               # Garbage collector
│   ├── ffi/              # Native FFI bridge
│   ├── distrib/          # Distributed computing (VDP)
│   ├── net/              # Networking (TCP/TLS)
│   ├── lsp/              # Language Server Protocol
│   ├── cli/              # CLI, VSH, REPL
│   ├── install/          # Installer
│   ├── util/             # Shared utilities
│   └── vesta_rt/         # Public C ABI stable runtime
├── include/              # Public headers
├── stdlib/native/        # Native standard library (plugins)
│   ├── io/               # I/O plugin
│   ├── math/             # Math plugin
│   ├── collections/      # Collections plugin
│   ├── runtime/          # Runtime introspection plugin
│   ├── photonic/         # Photonic (raytracing) plugin
│   ├── materia/          # Materia plugin
│   └── quantum/          # Quantum plugin
├── libs/SourceCode/      # Third-party dependencies
├── tests/                # Test suite (C++ files, one per subsystem)
├── examples_codes_vm/    # VM examples and demo plugins
├── examples_codes_vex/   # Vex language examples
└── doc/                  # Documentation
```

## Test Commands

```bash
# Run all tests
cmake --build build --parallel && ctest --test-dir build --output-on-failure --parallel

# Run tests with verbose output
ctest --test-dir build -V

# Run a single test category
ctest --test-dir build -R "test_runtime_"
```

## How to Add New Plugins

1. Create a directory under `examples_codes_vm/plugin_<name>/`
2. Add `CMakeLists.txt` using `include(VestaPlugin)` + `add_vesta_plugin()`:
   ```cmake
   cmake_minimum_required(VERSION 3.15)
   include(VestaPlugin)
   add_vesta_plugin(my_plugin
       SOURCES my_plugin.c
       SDK_DIR "${VESTA_SDK_DIR}"
   )
   ```
3. Implement `vesta_init()` as entry point:
   ```c
   #include "ffi/vesta_plugin.h"
   void vesta_init(const VestaPluginAPI *api) { ... }
   ```
4. Export functions with `uint64_t my_func(uint64_t a, uint64_t b)` signature
5. Add `add_subdirectory(examples_codes_vm/plugin_<name>)` to root `CMakeLists.txt`

## How to Add New Stdlib Modules

1. Create directory `stdlib/native/<name>/`
2. Add `CMakeLists.txt`:
   ```cmake
   cmake_minimum_required(VERSION 3.15)
   list(APPEND CMAKE_MODULE_PATH "${VESTA_SDK_DIR}/cmake")
   include(VestaPlugin)
   add_vesta_plugin(vesta_<name>
       SOURCES vesta_<name>.c
       SDK_DIR "${VESTA_SDK_DIR}"
   )
   ```
3. Add `add_subdirectory(<name>)` to `stdlib/native/CMakeLists.txt`
4. Register in `main.cpp` import tables

## Important Conventions

- **@Extern**: Marks a function as externally linked (FFI). Use in Vex source files to declare native functions from shared libraries or plugins.
- **@Macro**: Compile-time metaprogramming function. Evaluated at compile time; must return a string of Vex code that gets spliced into the call site.
- **@Foreign**: Declares external functions from non-Vesta libraries.
- **@Import**: Module import block for bytecode dependencies.
- **Calling convention**: Native functions receive `uint64_t` args in r1-r12, return in r0.
- **Plugin API**: `VestaPluginAPI` provides access to VM manager, logging, VM creation, memory allocation, and GC interaction.
- **Versioning**: Semantic versioning (`MAJOR.MINOR.PATCH`), exposed to Vex code via `@Target("compiler>=1.0")`.
- **Branching**: GitFlow-inspired — `release`, `develop`, `feature/*`, `refactor/*`, `hotfix/*`.
