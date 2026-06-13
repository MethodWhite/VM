# vexbindgen — Vex FFI Binding Generator

Automatically generates Vex `@Extern` bindings from C header files, similar to
[cbindgen](https://github.com/mozilla/cbindgen) or
[rust-bindgen](https://github.com/rust-lang/rust-bindgen).

## Usage

```bash
# Generate bindings to stdout
vexbindgen header.h

# Write to a file
vexbindgen header.h -o bindings.vex

# Process all .h files in a directory, output each as separate bindings
vexbindgen --module mylib include/ -o src/bindings/

# Disable wrapper generation, only emit @Extern declarations
vexbindgen --no-wrappers header.h -o ffi.vex
```

## Options

| Flag | Description |
|------|-------------|
| `-o`, `--output FILE` | Write output to file instead of stdout |
| `-m`, `--module NAME` | Set library module name for `@Extern("lib:func")` (default: `ffi`) |
| `-d`, `--dir` | Treat input as directory, process all `.h` files |
| `--no-wrappers` | Skip generating `@Inline` wrapper functions |
| `--no-consts` | Skip generating `const` declarations from `#define` macros |
| `--no-structs` | Skip generating `type` struct definitions |
| `--hashes` | Use `std.crypto.fnv1a`-based function name hashing |
| `-v`, `--verbose` | Enable verbose output with parse stats |
| `-h`, `--help` | Show help text |

## What it parses

### Function declarations

```c
// C input
void glClear(GLbitfield mask);
GLuint glCreateShader(GLenum shaderType);
const GLubyte* glGetString(GLenum name);
```

```vex
// Vex output
@Extern("opengl32:glClear")
fn glClear(mask: u32)

@Extern("opengl32:glCreateShader")
fn glCreateShader(shader_type: u32) -> u32

@Extern("opengl32:glGetString")
fn glGetString(name: u32) -> u64
```

### Struct definitions

```c
struct Vec3 {
    float x, y, z;
};
```

```vex
@Packed
type Vec3 = struct {
    x: f32
    y: f32
    z: f32
}
```

### Enums

```c
typedef enum {
    GL_TRIANGLES = 0x0004,
    GL_STATIC_DRAW = 0x88E4
} GLenum;
```

```vex
const GL_TRIANGLES : i32 = 0x0004
const GL_STATIC_DRAW : i32 = 0x88E4
```

### Type mappings

| C type | Vex type |
|--------|----------|
| `int` | `i32` |
| `long` | `i64` |
| `size_t` | `u64` |
| `float` | `f32` |
| `double` | `f64` |
| `char*` | `string` or `u64` |
| `void` | `()` |
| `void*` | `u64` |
| `T*` | `u64` (raw address) |
| `T[]` | `T[]` (with size) |
| `unsigned int` | `u32` |
| `struct Name` | `Name` |

## Architecture

```
vexbindgen/
  vexbindgen.vex     # Main CLI entry point, argument parsing, file I/O
  c_parser.vex       # C header tokenizer and parser
  vx_gen.vex         # Vex code generator (type mapping + output)
  examples/
    opengl_bindings.vex  # Example generated OpenGL bindings
  README.md
```

### Modules

- **c_parser.vex** — Tokenizes C source, parses function declarations, struct
  definitions, typedefs, enums, and `#define` constants. Skips preprocessor
  directives, comments, and complex type constructs.

- **vx_gen.vex** — Maps parsed C types to Vex types and generates the
  `@Extern("lib:name")` declarations. Can optionally generate `@Inline` wrapper
  functions with type-safe signatures.

- **vexbindgen.vex** — CLI entry point that dispatches arguments, coordinates
  parser and generator, handles file I/O.

## Example: OpenGL bindings

```vex
// Import generated bindings
use "opengl_bindings.vex" as gl

fn draw_triangle() {
    gl.Clear(gl.GL_COLOR_BUFFER_BIT | gl.GL_DEPTH_BUFFER_BIT)

    let mut vao: u32 = 0
    gl.GenVertexArrays(1, &vao)
    gl.BindVertexArray(vao)

    gl.DrawArrays(gl.GL_TRIANGLES, 0, 3)
}
```

## License

Part of the Vex VM project.
