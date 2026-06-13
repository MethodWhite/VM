# SolidScript — Graphics DSL for VestaVM

SolidScript is a domain-specific language for GPU-style shader programming,
embedded directly in Vex via the `@Solid` compile-time macro.  Instead of
writing raw shader string or using an external compiler, you write
SolidScript blocks that are **parsed and transformed to Vex IR at
compile-time**, using Vex's `@Macro` metaprogramming system.

## How SolidScript Works

SolidScript is not a separate language — it is a **Vex `@Macro`** defined in
`std.graphics.solid_macro`.  When the Vex compiler encounters:

```vex
@Solid vertex {
    input:   { position: float3, normal: float3, uv: float2 }
    output:  { position: float4, color: float3 }
    uniform: { model: matrix4x4, view: matrix4x4, proj: matrix4x4 }

    fn main() {
        output.position = proj * view * model * float4(input.position, 1.0)
        output.color = float3(0.5, 0.8, 1.0)
    }
}
```

The `@Solid` macro:

1. **Parses** the block type (`vertex`) and body as compile-time strings
2. **Extracts** the `input:`, `output:`, `uniform:`, and `fn main()` sections
3. **Transforms** the SolidScript declarations and body into equivalent Vex
   AST that constructs a `VertexShader` struct from `std.graphics`
4. **Injects** the resulting Vex code at the call site — zero runtime cost
   for parsing

The entire transformation happens at **compile time** via Vex's ComptimeVM,
following the same pattern as other `@Macro` functions (see
`examples_codes_vex/152_macros_inject_at_macro.vex`).

## Block Types

### `@Solid vertex { ... }`

Defines a vertex shader.  Transforms to a `VertexShader` struct with
input/output layouts and source.

| Section   | Description                                |
|-----------|--------------------------------------------|
| `input`   | Per-vertex attributes passed from the mesh |
| `output`  | Values interpolated and passed to fragment |
| `uniform` | Per-draw constants (matrices, etc.)        |
| `fn main` | Vertex transformation body                 |

The emitted `VertexShader` is assigned to a local variable that can be
used in a `Material` or passed to `compile_shader()`.

### `@Solid fragment { ... }`

Defines a fragment (pixel) shader.  Transforms to a `FragmentShader` struct.

| Section   | Description                              |
|-----------|------------------------------------------|
| `input`   | Interpolated values from vertex shader   |
| `output`  | Final pixel color                        |
| `uniform` | Per-draw constants (lights, etc.)        |
| `fn main` | Fragment shading body                    |

### `@Solid compute { ... }`

Defines a compute shader for GPGPU-style workloads.  Transforms to a
`ComputeShader` struct.

| Section      | Description                          |
|--------------|--------------------------------------|
| `uniform`    | Read-only input data                 |
| `workgroup`  | Thread group dimensions (x, y, z)    |
| `fn main`    | Compute kernel body                  |

### `@Solid material { ... }`

Defines a complete material combining a vertex and fragment shader with
physical properties.

| Section      | Description                          |
|--------------|--------------------------------------|
| `vertex:`    | Inline vertex shader block           |
| `fragment:`  | Inline fragment shader block         |
| `properties:`| PBR material properties              |

### `@Solid pipeline { ... }`

Defines a render pipeline configuration.  Transforms to a `PipelineConfig`
struct.

| Section      | Description                          |
|--------------|--------------------------------------|
| `stages:`    | Comma-separated stage names          |
| `viewport:`  | Resolution as `WIDTHxHEIGHT`         |

## Writing SolidScript Code

### Vertex Shader Example

```vex
@Solid vertex {
    input:   { position: float3, normal: float3, uv: float2 }
    output:  { position: float4, color: float3, normal: float3, uv: float2 }
    uniform: { model: matrix4x4, view: matrix4x4, proj: matrix4x4 }

    fn main() {
        output.position = proj * view * model * float4(input.position, 1.0)
        output.normal   = normalize((view * model) * float4(input.normal, 0.0)).xyz
        output.uv       = input.uv
        output.color    = float3(1.0, 1.0, 1.0)
    }
}
```

### Fragment Shader with Lighting

```vex
@Solid fragment {
    input:   { position: float4, color: float3, normal: float3, uv: float2 }
    output:  { color: float4 }
    uniform: { light_dir: float3, light_color: float3, ambient: float3 }

    fn main() {
        let n = normalize(input.normal)
        let l = normalize(light_dir)
        let diff = max(dot(n, l), 0.0)
        let spec = pow(max(dot(reflect(-l, n), float3(0, 0, 1)), 0.0), 32.0)
        output.color = float4(ambient + diff * light_color * input.color + spec * 0.5, 1.0)
    }
}
```

## Compilation to Vex IR

At compile-time, the `@Solid` macro:

1. Receives the block body as a `comptime string`
2. Calls internal parsing macros (`solid_extract_input`, `solid_extract_output`,
   `solid_extract_main`, etc.) to decompose the DSL
3. Calls `solid_emit_vertex` / `solid_emit_fragment` / etc. which produce
   Vex code constructing the appropriate `std.graphics` struct
4. The emitted string is parsed by Vex's parser and injected into the
   current compilation unit

The resulting Vex IR is identical to hand-written code constructing
`VertexShader`, `FragmentShader`, etc. structs — there is no runtime
representation of "SolidScript" itself.  The macro is **fully erased**.

## Integration with VestaVM's JIT

Because SolidScript blocks compile down to regular Vex struct values,
the VestaVM JIT can optimize shader code naturally:

- **Shader source** stored in the struct is passed to
  `std.graphics.compile_shader()` which triggers JIT code generation
  of the vertex/fragment/compute kernel
- **Matrix/vector math** in uniforms maps to Vex `f32` operations that
  the JIT lowers to SSE/AVX instructions
- **Pipeline state** encoded in `PipelineConfig` is read by the software
  rasterizer (or GPU backend) to configure the rendering loop
- The `@Extern` FFI bridge (`vesta_graphics:*`) connects Vex shader
  structs to native C++ renderer code loaded by the JIT runtime

## See Also

- `examples_codes_vex/solid_basic.vex` — basic triangle with SolidScript
- `examples_codes_vex/solid_photonic.vex` — photonic-driven real-time shader
- `std.graphics` — core graphics types (float2-4, matrix4x4, shaders, pipeline)
- `examples_codes_vex/152_macros_inject_at_macro.vex` — how `@Macro` works
- `examples_codes_vex/158_macros_lowered_to_ir.vex` — macro IR lowering
