# Vex Ecosystem Standards Documentation

Este documento proporciona una visión general completa de los estándares implementados para el ecosistema Vex, basados en las mejores prácticas de Synapsis y Oura.

## Índice

1. [Visión general](#1-visión-general)
2. [Configuración Vex](#2-configuración-vex)
3. [Sistema de testing](#3-sistema-de-testing)
4. [Documentación Vex](#4-documentación-vex)
5. [Scripts de construcción](#5-scripts-de-construcción)
6. [Convenciones de implementación](#6-convenciones-de-implementación)
7. [Referencias](#7-referencias)

---

## 1. Visión general

El ecosistema Vex ha implementado un conjunto completo de estándares basados en las mejores prácticas de Synapsis y Oura. Estos estándares proporcionan:

- **Configuración unificada**: Sistema de configuración TOML consistente para todos los componentes
- **Testing estructurado**: Suite de tests completa con patrones y herramientas
- **Documentación completa**: Documentación detallada siguiendo convenciones consistentes
- **Scripts de construcción**: Scripts de construcción y setup robustos y mantenibles
- **Convenciones de implementación**: Guías de estilo y mejores prácticas para desarrolladores

Los estándares están diseñados para:

- Facilitar el onboarding de nuevos contribuidores
- Asegurar la calidad del código a través de tests y documentación consistentes
- Proporcionar una experiencia de desarrollo uniforme
- Mantener el ecosistema sincronizado y bien mantenido

---

## 2. Configuración Vex

### 2.1 Arquivo de configuración principal

**Ubicación**: `vex.toml` (raíz del proyecto)

**Propósito**: Configuración centralizada para todos los componentes del ecosistema Vex.

**Secciones principales**:

- `[general]`: Configuración general del proyecto
- `[language]`: Configuración del lenguaje Vex
- `[runtime]`: Configuración del runtime (interp, JIT, memoria)
- `[distributed]`: Configuración de computación distribuida (VDP)
- `[debugging]`: Configuración de debugging y profiling
- `[plugins]`: Configuración de plugins del stdlib
- `[benchmarking]`: Configuración de benchmarking
- `[testing]`: Configuración de testing
- `[logging]`: Configuración de logging
- `[security]`: Configuración de seguridad

### 2.2 Valores por defecto

```toml
# Valores por defecto para referencia
[general]
nombre = "VestaVM"
versión = "1.0.0"
autor = "Equipo VestaVM"

[language]
dialecto = "vex"
backend = "velb"
optimizaciones = true

[runtime]
modo = "interp"
jit_threshold = 1000
max_memory_mb = 2048
gc_generation = 2

[distributed]
habilitado = true
protocolo = "vdp"
autenticación = "mtls"
```

### 2.3 Uso

```bash
# Cargar configuración desde archivo
vsta --config vex.toml

# Usar configuración por defecto
vsta  # carga automáticamente vex.toml o crea uno nuevo

# Sobrescribir valores desde línea de comandos
vsta --runtime.modo jit --runtime.max_memory_mb 4096
```

---

## 3. Sistema de testing

### 3.1 Arquitectura de testing

El sistema de testing Vex sigue una estructura jerárquica:

```
tests/
├── README.md                    # Documentación de la suite de tests
├── CMakeLists.txt               # Configuración de CMake para tests
├── vex/                         # Tests del lenguaje Vex
│   ├── test_vex_*.cpp          # Tests unitarios Vex
│   ├── test_vex_e2e.sh         # Tests E2E del lenguaje
│   └── test_vex_benchmarks.sh  # Tests de performance
├── runtime/                     # Tests del runtime
│   ├── test_runtime_*.cpp      # Tests unitarios del runtime
│   └── test_gc_*.cpp           # Tests del garbage collector
├── jit/                         # Tests del JIT
│   ├── test_jit_*.cpp          # Tests unitarios del JIT
│   └── test_jit_integration.cpp # Tests de integración del JIT
├── distributed/                 # Tests distribuidos
│   ├── test_vdp_*.cpp          # Tests del protocolo VDP
│   └── test_rspawn_*.cpp       # Tests de rspawn
├── plugins/                     # Tests de plugins
│   ├── test_plugin_*.cpp       # Tests unitarios de plugins
│   └── test_plugin_integration.cpp # Tests de integración de plugins
├── integration/                 # Tests de integración
│   ├── test_cli_*.cpp          # Tests de CLI
│   └── test_repl_*.cpp         # Tests del REPL
├── edge_cases/                 # Tests de casos extremos
│   ├── test_borrow_checker.cpp # Tests del borrow checker
│   └── test_memory_safety.cpp  # Tests de seguridad de memoria
└── benchmarks/                 # Tests de performance
    ├── run_all_benches.py      # Runner de benchmarks
    └── bench_results/          # Resultados de benchmarks
```

### 3.2 Tipos de tests

- **Unitarios**: Tests de componentes individuales (lexer, parser, typechecker, etc.)
- **Integración**: Tests que verifican componentes trabajando juntos
- **E2E**: Tests de programas completos (ejemplos Vex)
- **Borrow Checker**: Tests específicos para el borrow checker estilo Rust
- **Casos Extremos**: Tests de seguridad de memoria y manejo de errores
- **Benchmarks**: Tests de performance y medición

### 3.3 Configuración de testing

**Ubicación**: `tests/vex/test_config.toml`

**Propósito**: Configuración detallada para el sistema de testing.

**Secciones principales**:

- `[testing]`: Configuración general del runner de tests
- `[testing.unitario]`: Configuración de tests unitarios
- `[testing.integracion]`: Configuración de tests de integración
- `[testing.e2e]`: Configuración de tests E2E
- `[testing.benchmarks]`: Configuración de tests de benchmarks
- `[testing.borrow_checker]`: Configuración de tests de borrow checker
- `[testing.edge_cases]`: Configuración de tests de casos extremos
- `[testing.runner]`: Configuración del corredor de tests
- `[testing.reports]`: Configuración de reportes

### 3.4 Scripts de testing

- **`scripts/build_vex.sh`**: Script de construcción completo con configuración
- **`scripts/setup_dev.sh`**: Script de setup para desarrollo
- **`scripts/run_tests.sh`**: Runner de tests con soporte para múltiples modos
- **`tests/vex/test_vex_e2e.sh`**: Script de tests E2E para ejemplos Vex

---

## 4. Documentación Vex

### 4.1 Archivo de estándares

**Ubicación**: `doc/DOCUMENTATION_STANDARDS.md`

**Propósito**: Define las convenciones de documentación para todo el ecosistema Vex.

### 4.2 Estructura de documentación

La documentación Vex sigue una estructura jerárquica:

```
doc/
├── README.md                    # Documentación principal del proyecto
├── QUICKSTART.md                # Guía de inicio rápido (5 minutos)
├── LANGUAGE.md                   # Referencia completa del lenguaje Vex
├── ARCHITECTURE.md               # Arquitectura interna de VestaVM
├── BENCHMARKS.md                 # Resultados y metodología de benchmarks
├── ROADMAP.md                    # Plan de fases y estado actual
├── CONTRIBUTING.md               # Guía para contribuidores
├── DEPENDENCIES.md                # Dependencias y setup
├── SECURITY.md                   # Seguridad y mejores prácticas
├── LICENSE.md                    # Licencia
├── VMdoc/                        # Documentación técnica detallada
│   ├── Vex/                       # Documentación del lenguaje Vex
│   │   ├── TiposDatos.md          # Tipos de datos y primitivas
│   │   ├── Operadores.md          # Operadores y precedencia
│   │   ├── ControlFlow.md         # Control de flujo y patrones
│   │   ├── Strings.md             # Tipo string e interpolación
│   │   ├── OptionalResult.md     # Optional y Result
│   │   ├── Closures.md           # Lambdas y capturas
│   │   ├── OOP.md                # Programación orientada a objetos
│   │   ├── Generics.md            # Genéricos y monomorphización
│   │   ├── ReflexionAOP.md       # Reflexión y AOP
│   │   ├── Metaprogramacion.md   # Macros y metaprogramación
│   │   ├── Colecciones.md         # Colecciones estándar
│   │   └── Excepciones.md        # Manejo de excepciones
│   ├── IR/                        # Representación intermedia
│   │   └── SSA.md                # SSA IR y optimizaciones
│   ├── SetInstruccionesVM/        # Referencia del bytecode
│   │   ├── ALU.md               # Operaciones aritméticas
│   │   ├── MOV.md               # Operaciones de movimiento
│   │   └── ...                  # Más familias de opcodes
│   ├── runtime/                  # Runtime y scheduler
│   ├── Generics.md               # Generics en profundidad
│   ├── Debug.md                 # Debugger y debugging
│   ├── Hilos.md                 # Concurrencia y procesos
│   └── Distribuido.md            # VDP y computación distribuida
│
├── examples_codes_vex/            # Ejemplos del lenguaje
│   ├── benchmark/                # Ejemplos de benchmarks
│   └── ...                       # Más ejemplos
│
├── tests/                        # Documentación de testing
│   └── vex/                      # Documentación de tests Vex
│       ├── test_config.toml      # Configuración de testing
│       └── test_vex_e2e.sh       # Script de tests E2E
│
└── tools/                        # Herramientas y scripts
    └── dbg_client.vsh           # Cliente del debugger VSH
```

### 4.3 Convenciones de documentación

- **Lenguaje**: Español ASCII (documentación oficial)
- **Estilo**: Claro, conciso, con ejemplos prácticos
- **Referencias**: Enlaces cruzados con `[enlace](ruta)`
- **Notas**: `> Nota:`, `> Advertencia:`, `> Referencia:`
- **Código**: Usar ```vex ``` para bloques de código Vex

---

## 5. Scripts de construcción

### 5.1 Script de construcción principal

**Ubicación**: `scripts/build_vex.sh`

**Propósito**: Script de construcción completo con configuración y opciones.

**Características principales**:

- Soporta múltiples modos de construcción (debug, release)
- Configuración basada en archivos (vex.toml)
- Soporte para tests y benchmarks
- Construcción paralela optimizada
- Generación de documentación

### 5.2 Script de setup para desarrollo

**Ubicación**: `scripts/setup_dev.sh`

**Propósito**: Script de setup para nuevos desarrolladores.

**Características principales**:

- Verifica dependencias del sistema
- Inicializa submódulos
- Instala dependencias según el OS
- Configura CMake con flags de desarrollo
- Ejecuta tests automáticamente

### 5.3 Script de runner de tests

**Ubicación**: `scripts/run_tests.sh`

**Propósito**: Runner de tests flexible con soporte para múltiples modos.

**Modos soportados**:

- `unitario`: Solo tests unitarios
- `integracion`: Solo tests de integración
- `e2e`: Solo tests E2E
- `benchmarks`: Solo tests de benchmarks
- `borrow_checker`: Solo tests de borrow checker
- `edge_cases`: Solo tests de casos extremos
- `completo`: Todos los tests habilitados (default)

---

## 6. Convenciones de implementación

### 6.1 Estilo de código

- **Indentación**: 4 espacios (no tabs)
- **Longitud de línea**: Máximo 100 caracteres
- **Nombres**: `snake_case` para variables y funciones, `PascalCase` para tipos y clases
- **Documentación**: Usar `///` para documentación de Rust, `/** */` para documentación de C++

### 6.2 Nombres de archivos

- **Vex**: `nombre_archivo.vex`
- **VestaShellScript**: `nombre_archivo.vsh`
- **Bytecode**: `nombre_archivo.velb`
- **Documentación**: `nombre_archivo.md`
- **Configuración**: `config.toml`, `vex.toml`

### 6.3 Convenciones de rutas

- **Vex examples**: `examples_codes_vex/nombre_ejemplo.vex`
- **Benchmarks**: `examples_codes_vex/benchmark/nombre_ejemplo.vex`
- **Tests**: `tests/vex/nombre_test.cpp`
- **Plugins**: `stdlib/native/nombre_plugin/`
- **Documentación**: `doc/VMdoc/Vex/nombre_seccion.md`

---

## 7. Referencias

### 7.1 Documentos principales

- **README.md**: Documentación principal del proyecto
- **doc/QUICKSTART.md**: Guía de inicio rápido (5 minutos)
- **doc/LANGUAGE.md**: Referencia completa del lenguaje Vex
- **doc/ARCHITECTURE.md**: Arquitectura interna de VestaVM
- **doc/BENCHMARKS.md**: Resultados y metodología de benchmarks
- **doc/ROADMAP.md**: Plan de fases y estado actual

### 7.2 Documentos técnicos

- **doc/VMdoc/Vex/**: Documentación detallada del lenguaje Vex
- **doc/VMdoc/IR/SSA.md**: SSA IR y optimizaciones
- **doc/VMdoc/SetInstruccionesVM/**: Referencia del bytecode VM
- **doc/VMdoc/runtime/**: Runtime y scheduler

### 7.3 Guías de contribución

- **doc/CONTRIBUTING.md**: Cómo contribuir al proyecto
- **doc/DEPENDENCIES.md**: Dependencias y setup
- **doc/SECURITY.md**: Seguridad y mejores prácticas

### 7.4 Repositorios relacionados

- **VMdoc**: Documentación en formato Obsidian-friendly: [github.com/desmonHak/VMdoc](https://github.com/desmonHak/VMdoc)
- **Issues**: Reportar problemas: [GitHub Issues](https://github.com/desmonHak/VM/issues)

---

## Conclusión

El ecosistema Vex ha implementado un conjunto completo de estándares basados en las mejores prácticas de Synapsis y Oura. Estos estándares proporcionan:

- **Configuración consistente**: Sistema de configuración TOML unificado
- **Testing exhaustivo**: Suite de tests completa con patrones y herramientas
- **Documentación completa**: Documentación detallada siguiendo convenciones consistentes
- **Scripts robustos**: Scripts de construcción y setup confiables y mantenibles
- **Convenciones claras**: Guías de estilo y mejores prácticas para desarrolladores

Estos estándares facilitan el onboarding de nuevos contribuidores, aseguran la calidad del código y mantienen el ecosistema sincronizado y bien mantenido.

Para más información, consulte la documentación principal y las guías de contribución.
