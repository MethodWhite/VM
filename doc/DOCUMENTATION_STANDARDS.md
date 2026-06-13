# Vex Ecosystem Documentation Standards

Este documento define los estándares de documentación para el ecosistema Vex, combinando las mejores prácticas de Synapsis y Oura.

## Índice

1. [Visión general](#1-visión-general)
2. [Estructura de documentación](#2-estructura-de-documentación)
3. [Convenciones de documentación](#3-convenciones-de-documentación)
4. [Formatos de documentación](#4-formatos-de-documentación)
5. [Herramientas de documentación](#5-herramientas-de-documentación)
6. [Convenciones de código](#6-convenciones-de-código)
7. [Generación de documentación](#7-generación-de-documentación)
8. [Ejemplos](#8-ejemplos)

---

## 1. Visión general

El sistema de documentación Vex está diseñado para:

- Proporcionar documentación completa y consistente para todos los componentes del ecosistema
- Soportar múltiples formatos (Markdown, HTML, PDF)
- Integrar con el sistema de ayuda de la CLI
- Facilitar la navegación y búsqueda de información
- Mantener la documentación sincronizada con el código
- Soportar contribuciones de documentación

El sistema está inspirado en:

- **Synapsis**: Documentación estructurada con referencias cruzadas, ejemplos, y guías de configuración
- **Oura**: Documentación API con referencias de tipos, ejemplos de uso, y documentación de comandos
- **Vex**: Documentación del lenguaje con referencias detalladas, ejemplos, y notas de versión

---

## 2. Estructura de documentación

La estructura de documentación Vex sigue una organización jerárquica:

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
│   ├── ...                       # Más ejemplos
│
├── tests/                        # Documentación de testing
│   ├── README.md                # Documentación de la suite de tests
│   └── vex/                      # Documentación de tests Vex
│       ├── test_config.toml      # Configuración de testing
│       └── test_vex_e2e.sh       # Script de tests E2E
│
└── tools/                        # Herramientas y scripts
    ├── dbg_client.vsh           # Cliente del debugger VSH
    └── ...                      # Más herramientas
```

---

## 3. Convenciones de documentación

### 3.1 Estilo de escritura

- **Lenguaje**: Español ASCII (documentación oficial del proyecto)
- **Claridad**: Ser conciso pero completo, evitar jerga innecesaria
- **Ejemplos**: Proporcionar ejemplos prácticos y completos
- **Referencias**: Usar referencias cruzadas con `[enlace](ruta)`
- **Notas**: Usar `> Nota:` para notas importantes, `> Advertencia:` para advertencias

### 3.2 Estructura de secciones

Cada documento debe seguir esta estructura:

```markdown
# Título del Documento

> Resumen breve (1-2 oraciones)

## Índice

- [Sección 1](#sección-1)
- [Sección 2](#sección-2)

## Sección 1: Título

Contenido de la sección.

### Subtítulo 1

Más detalles.

#### Subtítulo 2

Incluso más detalles.

## Sección 2: Otro Título

Más contenido.

### Ejemplos

```vex
// Código de ejemplo
i32 main() {
    println("Hola Mundo");
    return 0;
}
```

### Notas

> Nota: Información importante
> Advertencia: Peligro potencial
> Referencia: [Enlace a otra sección](#enlace)

## 4. Formatos de documentación

### 4.1 Markdown

- Usar GitHub Flavored Markdown
- Usar encabezados con `#` para títulos
- Usar `>` para citas
- Usar `**negrita**` para énfasis
- Usar `*cursiva*` para énfasis
- Usar `` `código` `` para código en línea
- Usar ```bloque de código ``` para bloques de código

### 4.2 HTML

- Generar desde Markdown usando MkDocs o Docusaurus
- Usar navegación lateral para documentos anidados
- Incluir tabla de contenidos para documentos largos
- Usar resaltado de sintaxis para código

### 4.3 PDF

- Generar desde Markdown usando pandoc
- Incluir tabla de contenidos, índices
- Usar fuentes legibles (DejaVu Sans, Libertinus Mono)

---

## 5. Herramientas de documentación

### 5.1 Generadores de documentación

- **MkDocs**: Para sitios web estáticos
- **Docusaurus**: Para documentación con navegación interactiva
- **Sphinx**: Para documentación técnica en Python
- **pandoc**: Para conversión entre formatos

### 5.2 Herramientas de ayuda

- **CLI con `--help`**: Proporcionar ayuda en línea para todos los comandos
- **`--vex-help`**: Proporcionar ayuda específica del lenguaje Vex
- **`--docs`**: Abrir la documentación en el navegador predeterminado
- **`--generate-docs`**: Generar documentación desde el código fuente

### 5.3 Validación

- **vale**: Para validación de estilo de Markdown
- **markdownlint**: Para linting de Markdown
- **linkinator**: Para verificar enlaces rotos
- **markdown-link-check**: Para verificación de enlaces

---

## 6. Convenciones de código

### 6.1 Estilo de código

- **Indentación**: 4 espacios (no tabs)
- **Longitud de línea**: Máximo 100 caracteres
- **Nombres**: `snake_case` para variables y funciones, `PascalCase` para tipos y clases
- **Comentarios**: Usar `//` para comentarios de línea, `/* */` para comentarios de bloque
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

## 7. Generación de documentación

### 7.1 Documentación automática

- **Referencias de tipos**: Generar desde anotaciones de código
- **Diagramas**: Generar diagramas Mermaid del pipeline de compilación
- **Referencia de API**: Generar desde docstrings
- **Ejemplos**: Extraer ejemplos del código fuente

### 7.2 Pipeline de documentación

```
Código fuente → Parser → AST → Generador de documentación
    ↓
Documentación de tipos → Referencias cruzadas → Estructura de navegación
    ↓
Markdown → HTML/PDF → Sitio web estático
```

### 7.3 Mantenimiento de documentación

- **Sincronización automática**: Mantener la documentación sincronizada con el código
- **Validación**: Verificar que la documentación esté actualizada
- **Revisión**: Revisar documentación en cada PR
- **Actualización**: Actualizar documentación cuando el código cambia

---

## 8. Ejemplos

### 8.1 Ejemplo de documento Vex

```markdown
# TiposDatos

Tipos de datos primitivos y compuestos del lenguaje Vex.

## Índice

- [Primitivos](#primitivos)
- [Punteros](#punteros)
- [Structs](#structs)
- [Enums](#enums)
- [Strings](#strings)

## Primitivos

Vex soporta los siguientes tipos de datos primitivos:

```vex
i8   // entero de 8 bits con signo
i16  // entero de 16 bits con signo
i32  // entero de 32 bits con signo (default)
i64  // entero de 64 bits con signo

u8   // entero de 8 bits sin signo
u16  // entero de 16 bits sin signo
u32  // entero de 32 bits sin signo
u64  // entero de 64 bits sin signo

f32  // flotante de 32 bits
f64  // flotante de 64 bits (default)

bool // booleano
char // carácter UTF-8
```

### Primitivos con signo vs sin signo

Los tipos con signo (`i*`) y sin signo (`u*`) tienen diferentes rangos:

- **Con signo**: `-2^(n-1)` a `2^(n-1)-1`
- **Sin signo**: `0` a `2^n-1`

### Uso de tipos

```vex
i32 entero = 42;
u32 sin_signo = 100;
f64 flotante = 3.14159;
bool bandera = true;
char letra = 'A';
```

### Inferencia de tipos

Vex soporta inferencia de tipos local:

```vex
auto x = 42;        // i32
auto y = 3.14;       // f64
auto z = "hola";     // string
```

## Punteros

Vex soporta tres tipos de punteros:

### Punteros raw

```vex
i32 v = 42;
i32* p = &v;      // puntero raw a memoria HOST
*p = 100;           // desreferenciar
```

### Punteros virtuales

```vex
VirtualPtr<i32> vp = ...;  // puntero a memoria virtual VM
*i32 valor = *vp;           // desreferenciar
```

### Smart pointers

```vex
unique<i32> uptr = unique_box(42);  // ownership
i32 valor = *uptr;                  // desreferenciar
```

## Structs

```vex
struct Punto {
    i32 x;
    i32 y;
    
    Punto(i32 xx, i32 yy) {
        this.x = xx;
        this.y = yy;
    }
};

Punto origen = Punto(0, 0);
```

## Enums

```vex
enum Color {
    Rojo(i32),
    Verde(i32),
    Azul(i32)
};

Color c = Color.Rojo(255);
```

## Strings

```vex
string mensaje = "Hola Mundo";
string interpolado = "Valor: ${42}";
```

Para más información, consulte:

- [Operadores](./Operadores.md) - Operadores para strings
- [Strings](./Strings.md) - Tipo string y operaciones
- [Colecciones](./Colecciones.md) - String operations en colecciones
```

### 8.2 Ejemplo de configuración

```toml
# vex.toml

[general]
nombre = "Mi Proyecto Vex"
versión = "1.0.0"
autor = "Desarrollador"

[language]
dialecto = "vex"
backend = "velb"
optimizaciones = true

[runtime]
modo = "interp"
jit_threshold = 1000
max_memory_mb = 2048

[debugging]
habilitado = true
nivel = "debug"

[testing]
habilitado = true
rutas = ["tests/vex/", "examples_codes_vex/"]

[benchmarking]
habilitado = true
directorio = "bench_results"
```

### 8.3 Ejemplo de documento de testing

```markdown
# test_vex_e2e.sh

> Ejecuta tests E2E para ejemplos Vex.

## Propósito

Este script ejecuta todos los ejemplos del repositorio (`examples_codes_vex/`) para verificar que el compilador Vex genera código correcto y que el runtime ejecuta programas correctamente.

## Uso

```bash
# Ejecutar tests E2E
cd tests/vex
./test_vex_e2e.sh

# Ejecutar con configuración personalizada
TEST_CONFIG=../test_config.toml ./test_vex_e2e.sh
```

## Comandos principales

1. **Compilar**: `./build/vm --vex ejemplo.vex -o ejemplo`
2. **Ejecutar**: `./build/vm --run ejemplo.velb`
3. **Verificar borrow checker**: `./build/vm --vex ejemplo.vex --check-borrow`

## Notas de implementación

- El script compila cada ejemplo Vex a bytecode `.velb`
- Luego ejecuta cada archivo `.velb` para verificar la ejecución correcta
- Finalmente verifica el borrow checker para detectar problemas de seguridad de memoria
- Todos los tests deben pasar para que el script retorne éxito
```

---

## Conclusión

Los estándares de documentación Vex proporcionan un marco consistente para documentar el ecosistema Vex, asegurando que la documentación sea completa, accesible y mantenida al día con el código. Siguiendo estos estándares, los contribuidores pueden crear documentación de alta calidad que facilita el uso y mantenimiento del proyecto.

Para más información, consulte:

- [CONTRIBUTING.md](./CONTRIBUTING.md) - Guía para contribuidores
- [DEPENDENCIES.md](./DEPENDENCIES.md) - Dependencias y setup
- [SECURITY.md](./SECURITY.md) - Seguridad y mejores prácticas
