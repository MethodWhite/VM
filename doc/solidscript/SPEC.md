# SolidScript — Language Specification

**Version:** 0.1  
**Status:** Draft  
**Compilation target:** Vex SSA IR (via `@Solid` macro)  
**Runtime:** VestaVM

---

## Table of Contents

1. [Philosophy](#1-philosophy)
2. [Pipeline](#2-pipeline)
3. [Types](#3-types)
4. [Syntax](#4-syntax)
5. [Built-in Functions](#5-built-in-functions)
6. [Shader Patterns](#6-shader-patterns)
7. [Material System](#7-material-system)
8. [Scene Description](#8-scene-description)
9. [Integration with Vex](#9-integration-with-vex)
10. [Standard Library](#10-standard-library)

---

## 1. Philosophy

SolidScript is a **domain-specific language for shading, graphics scripting, and scene description** that lives entirely inside Vex. It is not a separate compiler — it is a **Vex metaprogram** that transforms high-level graphics DSL syntax into Vex SSA IR at compile time via the `@Solid` annotation macro.

### Design tenets

1. **All SolidScript is valid Vex.** Every `.solid` file is syntactically valid Vex. The `@Solid` macro rewrites DSL constructs into Vex IR during the VPP preprocessor + macro expansion phase. There is no separate lexer, parser, or type checker.

2. **GPU-ready semantics.** Types like `float3`, `matrix4x4`, `texture`, `sampler`, `shader`, `material`, `mesh`, and `light` are first-class Vex constructs. They lower to optimized struct layouts and FFI calls to the underlying graphics API (Vulkan/Metal/DirectX) via VestaVM's native call system.

3. **Shaders as pure functions.** A vertex, fragment, or compute shader is a `@Macro`-annotated Vex function that returns generated pipeline code. The `@Solid` macro collects all shader stages, links them, and emits a render pipeline descriptor.

4. **Zero-cost abstraction.** All SolidScript constructs are resolved at compile time. The emitted bytecode contains only flat SSA IR with no runtime reflection overhead.

5. **Graphics pipeline as code.** SolidScript treats the entire rendering pipeline — vertex input, shader stages, rasterization state, depth/stencil, blend — as programmable Vex code, not configuration files.

---

## 2. Pipeline

```
.solid source
    |
    v  VPP preprocessor (#import, #define, #if)
    |
    v  Lexer + Parser Vex (standard Vex frontend)
    |
    v  @Solid macro expansion (compile-time)
    |   - Transforms DSL types to Vex structs
    |   - Lowers shader patterns to @Macro-generated IR
    |   - Emits render pipeline descriptors as Vex constants
    |   - Generates FFI bindings to graphics API
    |
    v  Type Checker (standard Vex type checker)
    |
    v  Lowering (AST -> SSA IR)
    |
    v  IR Optimizer (15+ passes)
    |
    v  Assembler + Linker -> .velb bytecode
    |
    v  VestaVM runtime
         |
         +---> Graphics API (Vulkan/Metal/DX12 via FFI)
         +---> GPU shader compilation (SPIR-V/HLSI/MSL generated at FFI boundary)
         +---> Render pipeline execution
```

The pipeline is intentionally the **same as Vex**. The `@Solid` macro operates during the macro expansion phase of the VPP preprocessor, transforming SolidScript DSL constructs into plain Vex before the type checker ever sees them.

### The `@Solid` macro

```vex
@Solid
#import <solid/std>

float3 color = float3(1.0, 0.0, 0.0);
```

The `@Solid` annotation placed at the top of a file (or a scope) instructs the preprocessor to:

1. `#import` the SolidScript standard library (`<solid/std>`, `<solid/math>`, `<solid/pipeline>`)
2. Enable DSL type aliases (`float3` → `SolidFloat3`, etc.)
3. Activate shader pattern recognition for `vertex`, `fragment`, `compute` blocks
4. Register the file as a graphics module

---

## 3. Types

### 3.1 Vectors

| SolidScript | Vex lowering | Size | Description |
| :---------- | :----------- | :--- | :---------- |
| `float2` | `SolidFloat2` | 8 B | 2-component float vector |
| `float3` | `SolidFloat3` | 12 B | 3-component float vector |
| `float4` | `SolidFloat4` | 16 B | 4-component float vector (aligned to 16) |

```vex
// float2 lowering
struct SolidFloat2 {
    x: f32,
    y: f32,
}

// float3 lowering
struct SolidFloat3 {
    x: f32,
    y: f32,
    z: f32,
}

// float4 lowering
struct SolidFloat4 {
    x: f32,
    y: f32,
    z: f32,
    w: f32,
}
```

**Swizzling** is supported via compile-time `@Macro` expansion:

```solid
float4 v = float4(1.0, 2.0, 3.0, 4.0);
float2 rg = v.xy;       // expands to SolidFloat2 { v.x, v.y }
float3 xyz = v.xyz;     // expands to SolidFloat3 { v.x, v.y, v.z }
float4 wyzx = v.wyzx;   // arbitrary swizzle patterns
```

All standard swizzle combinations (xy, xyz, xyzw, rgba, stpq) are predefined.

### 3.2 Matrix

| SolidScript | Vex lowering | Size | Description |
| :---------- | :----------- | :--- | :---------- |
| `matrix4x4` | `SolidMatrix4x4` | 64 B | 4×4 column-major float matrix |

```vex
struct SolidMatrix4x4 {
    cols: [SolidFloat4; 4],    // 4 columns, each a float4
}
```

Constructors:

```solid
matrix4x4 m = matrix4x4::identity();
matrix4x4 m = matrix4x4::translate(float3(1, 0, 0));
matrix4x4 m = matrix4x4::rotate_x(f32 angle);
matrix4x4 m = matrix4x4::rotate_y(f32 angle);
matrix4x4 m = matrix4x4::rotate_z(f32 angle);
matrix4x4 m = matrix4x4::scale(float3(2, 2, 2));
matrix4x4 m = matrix4x4::perspective(f32 fov, f32 aspect, f32 near, f32 far);
matrix4x4 m = matrix4x4::look_at(float3 eye, float3 target, float3 up);
```

Operators:

```solid
matrix4x4 c = a * b;        // matrix-matrix multiply
float4 v2 = m * float4(v);  // matrix-vector multiply
float3 v3 = m * float3(v);  // matrix-point multiply (w=1 implicit)
```

### 3.3 Color

| SolidScript | Vex lowering | Size | Description |
| :---------- | :----------- | :--- | :---------- |
| `color` | `SolidColor` | 16 B | RGBA float color |

```vex
struct SolidColor {
    r: f32,
    g: f32,
    b: f32,
    a: f32,
}
```

```solid
color red  = #FF0000;          // hex literal
color blue = color(0, 0, 1);   // RGB [0..1]
color rgba = color(1, 0, 0, 0.5); // RGBA [0..1]
color hsv  = color::hsv(0.5, 0.8, 1.0); // HSV → RGB conversion (comptime)
```

### 3.4 Texture & Sampler

```vex
// Opaque handle — resolves to graphics API texture object
@opaque struct SolidTexture {
    handle: u64,             // GPU-side handle
    width: u32,
    height: u32,
    depth: u32,
    mip_levels: u32,
    format: SolidTextureFormat,
}

// Opaque handle — resolves to graphics API sampler object
@opaque struct SolidSampler {
    handle: u64,
    min_filter: SolidFilter,
    mag_filter: SolidFilter,
    mip_filter: SolidFilter,
    address_u: SolidAddressMode,
    address_v: SolidAddressMode,
    address_w: SolidAddressMode,
}
```

```solid
texture t = texture::load("assets/diffuse.png");
sampler s = sampler::linear_clamp();

// Query texture properties
u32 w = t.width;
u32 h = t.height;
```

### 3.5 Shader

A `shader` in SolidScript is a **compile-time descriptor** that bundles vertex, fragment, and optional compute stages. It is not a runtime value — it is a Vex struct generated by the `@Solid` macro.

```vex
struct SolidShader {
    vertex_entry:   string,      // name of the vertex function
    fragment_entry: string,      // name of the fragment function
    compute_entry:  string,      // name of the compute function (optional)
    attributes:     [SolidVertexAttribute; ..],  // vertex input layout
    uniforms:       [SolidUniform; ..],          // uniform bindings
}
```

```solid
shader phong = shader {
    vertex:   vs_main,
    fragment: fs_main,
    attributes: {
        { binding: 0, format: Float3, name: "position" },
        { binding: 1, format: Float3, name: "normal"   },
        { binding: 2, format: Float2, name: "uv"       },
    },
    uniforms: {
        { binding: 0, stage: Vertex,   type: matrix4x4, name: "modelViewProj" },
        { binding: 1, stage: Fragment, type: float3,    name: "lightDir"     },
    },
};
```

### 3.6 Material

A `material` is a runtime object combining a `shader` with concrete resource bindings (textures, samplers, uniform values).

```vex
struct SolidMaterial {
    shader:    SolidShader,
    textures:  [SolidTextureBinding; ..],
    uniforms:  [SolidUniformValue; ..],
    blend:     SolidBlendState,
    depth:     SolidDepthState,
    raster:    SolidRasterState,
}
```

```solid
material mat = material {
    shader: phong,
    textures: {
        { binding: 0, texture: diffuse_map, sampler: linear_clamp },
    },
    uniforms: {
        { binding: 0, value: mvp_matrix },
        { binding: 1, value: light_dir   },
    },
    blend:   blend::opaque(),
    depth:   depth::less_equal(),
    raster:  raster::cull_back(),
};
```

### 3.7 Mesh

```vex
struct SolidMesh {
    vertex_buffer:   u64,        // GPU buffer handle
    index_buffer:    u64,        // GPU buffer handle (0 if non-indexed)
    vertex_count:    u32,
    index_count:     u32,
    vertex_stride:   u32,
    primitive:       SolidPrimitiveTopology,
    bounds:          SolidBoundingBox,
}
```

```solid
mesh triangle = mesh {
    vertices: {
        { position: float3(-0.5, -0.5, 0), normal: float3(0, 0, 1), uv: float2(0, 0) },
        { position: float3( 0.5, -0.5, 0), normal: float3(0, 0, 1), uv: float2(1, 0) },
        { position: float3( 0.0,  0.5, 0), normal: float3(0, 0, 1), uv: float2(0.5, 1) },
    },
    primitive: Triangles,
};
```

### 3.8 Light

```vex
struct SolidLight {
    kind:       SolidLightKind,     // Directional, Point, Spot
    intensity:  f32,
    color:      SolidColor,
    direction:  SolidFloat3,        // for directional/spot
    position:   SolidFloat3,        // for point/spot
    range:      f32,                // for point/spot
    inner_cone: f32,                // for spot
    outer_cone: f32,                // for spot
    shadows:    bool,
}
```

```solid
light sun = light::directional(
    color:      #FFF4E0,
    intensity:  2.0,
    direction:  float3(1, -1, 0.5),
    shadows:    true,
);

light lamp = light::point(
    color:      #FF8800,
    intensity:  5.0,
    position:   float3(2, 3, -1),
    range:      10.0,
);
```

---

## 4. Syntax

SolidScript is a **subset + superset** of Vex syntax. It uses the same C-like curly-brace syntax, the same type annotations, and the same macro system.

### 4.1 File extension

- `.solid` — SolidScript source file (processed by VPP with `@Solid`)
- `.solid.vph` — SolidScript header / macro library

### 4.2 Annotations

```solid
@Solid                          // Marks file/scope as SolidScript
@vertex                         // Marks function as vertex shader entry
@fragment                       // Marks function as fragment shader entry
@compute(local_size_x: 16, local_size_y: 16, local_size_z: 1)  // Compute shader
@binding(slot: 0, stage: Vertex)    // Uniform binding annotation
@input(binding: 0)              // Vertex input attribute
@output                         // Shader output (SV_Target equivalent)
@builtin                        // Marks a parameter as built-in input
@push_constant                  // Marks uniform block as push constant
@instanced                      // Marks vertex input as instanced
@specialization(constant_id: 0) // Specialization constant
```

### 4.3 Vertex shader

```solid
@vertex
float4 vs_main(
    @input(binding: 0) float3 position,
    @input(binding: 1) float3 normal,
    @input(binding: 2) float2 uv,
    @builtin(SV_VertexID) u32 vertex_id,
) {
    // ... return clip-space position
}
```

### 4.4 Fragment shader

```solid
@fragment
float4 fs_main(
    @builtin(SV_Position) float4 frag_coord,
    @builtin(SV_IsFrontFace) bool is_front,
) {
    // ... return color
}
```

### 4.5 Compute shader

```solid
@compute(local_size_x: 256, local_size_y: 1, local_size_z: 1)
void cs_main(
    @builtin(SV_GlobalInvocationID) uint3 id,
    @builtin(SV_LocalInvocationID) uint3 local_id,
) {
    // ... compute work
}
```

### 4.6 Built-in variables

| Built-in | Stage | Type | Description |
| :------- | :---- | :--- | :---------- |
| `SV_VertexID` | Vertex | `u32` | Vertex index |
| `SV_InstanceID` | Vertex | `u32` | Instance index |
| `SV_Position` | Vertex→Frag | `float4` | Clip-space position |
| `SV_IsFrontFace` | Fragment | `bool` | Front-face test |
| `SV_SampleIndex` | Fragment | `u32` | MSAA sample index |
| `SV_GlobalInvocationID` | Compute | `u32[3]` | Global workgroup ID |
| `SV_LocalInvocationID` | Compute | `u32[3]` | Local workgroup ID |
| `SV_WorkgroupID` | Compute | `u32[3]` | Workgroup index |
| `SV_DispatchThreadID` | Compute | `u32[3]` | Global thread ID |

### 4.7 Uniform bindings

```solid
@binding(slot: 0, stage: Vertex)
uniform matrix4x4 modelViewProj;

@binding(slot: 1, stage: Fragment)
uniform float3 lightDir;

@binding(slot: 2, stage: Fragment)
uniform texture diffuseMap;

@binding(slot: 3, stage: Fragment)
uniform sampler linearSampler;

@push_constant
uniform struct PushConstants {
    f32 time;
    f32 delta;
} pc;
```

### 4.8 Control flow

SolidScript supports the full Vex control flow:

```solid
if (depth < 0.5) {
    discard;
}

for (i32 i = 0; i < 4; i++) {
    total += samples[i];
}

while (ray.t < max_dist) {
    ray.march();
}
```

---

## 5. Built-in Functions

### 5.1 Math (swizzle-aware)

| Function | Description |
| :------- | :---------- |
| `dot(a, b)` | Dot product (float2/float3/float4) |
| `cross(a, b)` | Cross product (float3) |
| `length(v)` | Vector magnitude |
| `normalize(v)` | Unit vector |
| `reflect(i, n)` | Reflection vector |
| `refract(i, n, eta)` | Refraction vector |
| `lerp(a, b, t)` | Linear interpolation |
| `saturate(x)` | Clamp 0..1 |
| `step(edge, x)` | Heaviside step |
| `smoothstep(e0, e1, x)` | Hermite interpolation |
| `mix(a, b, t)` | Component-wise lerp |
| `clamp(x, lo, hi)` | Clamp value |
| `distance(a, b)` | Distance between points |
| `faceforward(n, i, ng)` | Orient normal |
| `fwidth(v)` | Sum of partial derivatives |
| `ddx(v)` | Screen-space X derivative |
| `ddy(v)` | Screen-space Y derivative |

All Vex built-in math functions (`sin`, `cos`, `sqrt`, `pow`, etc.) are automatically available.

### 5.2 Texture sampling

```solid
float4 sample = texture_sample(diffuseMap, linearSampler, uv);
float4 sample_bias = texture_sample_bias(diffuseMap, linearSampler, uv, mip_bias);
float4 sample_lod = texture_sample_lod(diffuseMap, linearSampler, uv, lod);
float4 sample_grad = texture_sample_grad(diffuseMap, linearSampler, uv, ddx_uv, ddy_uv);
float4 sample_cube = texture_sample_cube(skybox, linearSampler, direction);
float4 sample_array = texture_sample_array(tex_array, linearSampler, uv, array_index);
float sample_depth = texture_sample_depth(depthMap, pointSampler, uv);
```

### 5.3 Matrix operations

```solid
matrix4x4 m = matrix4x4::identity();
matrix4x4 m = matrix4x4::translate(float3 t);
matrix4x4 m = matrix4x4::rotate_axis(f32 angle, float3 axis);
matrix4x4 m = matrix4x4::scale(float3 s);
matrix4x4 m = matrix4x4::perspective(f32 fov, f32 aspect, f32 near, f32 far);
matrix4x4 m = matrix4x4::orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near, f32 far);
matrix4x4 m = matrix4x4::look_at(float3 eye, float3 target, float3 up);
matrix4x4 m_inv = inverse(m);
matrix4x4 m_t = transpose(m);
float3 translation = m.translation();
float3 scale_part = m.scale();
float3x3 rotation = m.rotation();
```

### 5.4 Noise and randomness

```solid
f32 n = noise::perlin(float3 p);           // Perlin noise
f32 n = noise::simplex(float3 p);          // Simplex noise
f32 n = noise::worley(float3 p);           // Worley (Voronoi) noise
f32 n = noise::fbm(float3 p, i32 octaves); // Fractal Brownian Motion
f32 r = random::uniform(float2 seed);      // GPU-friendly PRNG
float2 r2 = random::uniform2d(float2 seed);
```

### 5.5 Coordinate transforms

```solid
float4 clip = modelViewProj * float4(world_pos, 1.0);
float3 world = inverse(model) * float3(local_pos);
float3 view = normalize(eye - world_pos);
float3 ndc = clip.xyz / clip.w;
float2 screen = ndc_to_screen(ndc, viewport_size);
```

---

## 6. Shader Patterns

### 6.1 Vertex shader pattern

```solid
@vertex
ShaderOutput vs_main(VertexInput input) {
    ShaderOutput out;

    float4 world_pos = model * float4(input.position, 1.0);
    out.position = viewProj * world_pos;
    out.normal   = normalize((transpose(inverse(model))) * float4(input.normal, 0.0)).xyz;
    out.uv       = input.uv;
    out.world_pos = world_pos.xyz;

    return out;
}
```

The `@vertex` annotation causes the `@Solid` macro to:

1. Generate a Vex `@Macro` that emits a vertex shader entry point
2. Create the vertex input layout descriptor from `@input` annotations
3. Wire the output struct as the fragment shader input
4. Generate the FFI call to `vkCreateShaderModule` / equivalent at pipeline creation

### 6.2 Fragment shader pattern

```solid
@fragment
float4 fs_main(ShaderInput input) {
    float3 N = normalize(input.normal);
    float3 L = normalize(light.direction);
    float3 V = normalize(eye_pos - input.world_pos);

    float3 diffuse = max(dot(N, L), 0.0) * light.color * light.intensity;
    float3 ambient = 0.05 * base_color;
    float3 final = ambient + diffuse;

    return float4(final, 1.0);
}
```

### 6.3 Compute shader pattern

```solid
@compute(local_size_x: 64, local_size_y: 1, local_size_z: 1)
void cs_main(@builtin(SV_GlobalInvocationID) uint3 id) {
    u32 index = id.x;

    if (index >= particle_count) return;

    Particle p = particles[index];
    p.velocity += gravity * delta_time;
    p.position += p.velocity * delta_time;

    if (p.position.y < ground_y) {
        p.position.y = ground_y;
        p.velocity *= float3(0.8, -0.5, 0.8);
    }

    particles[index] = p;
}
```

### 6.4 Full pipeline shader

```solid
@Solid

#import <solid/std>
#import <solid/math>

// ====== Vertex input structure ======
struct VertexInput {
    @input(binding: 0) float3 position;
    @input(binding: 1) float3 normal;
    @input(binding: 2) float2 uv;
}

// ====== Inter-stage structure ======
struct ShaderOutput {
    @builtin(SV_Position) float4 position;
    float3 normal;
    float2 uv;
    float3 world_pos;
}

// ====== Uniforms ======
@binding(slot: 0, stage: Vertex)
uniform matrix4x4 modelViewProj;

@binding(slot: 1, stage: Vertex)
uniform matrix4x4 model;

@binding(slot: 2, stage: Fragment)
uniform float3 lightDir;

@binding(slot: 3, stage: Fragment)
uniform float3 lightColor;

@binding(slot: 4, stage: Fragment)
uniform texture baseTexture;

@binding(slot: 5, stage: Fragment)
uniform sampler baseSampler;

// ====== Vertex shader ======
@vertex
ShaderOutput vs_main(VertexInput input) {
    ShaderOutput out;

    float4 world_pos = model * float4(input.position, 1.0);
    out.position  = modelViewProj * float4(input.position, 1.0);
    out.normal    = normalize(
        (transpose(inverse(model))) * float4(input.normal, 0.0)
    ).xyz;
    out.uv        = input.uv;
    out.world_pos = world_pos.xyz;

    return out;
}

// ====== Fragment shader ======
@fragment
float4 fs_main(ShaderOutput input) {
    float4 tex_color = texture_sample(baseTexture, baseSampler, input.uv);

    float3 N = normalize(input.normal);
    float3 L = normalize(lightDir);
    float NdotL = max(dot(N, L), 0.0);

    float3 diffuse = NdotL * lightColor * tex_color.rgb;
    float3 ambient = 0.05 * tex_color.rgb;

    return float4(diffuse + ambient, tex_color.a);
}
```

---

## 7. Material System

Materials in SolidScript are **runtime resource bundles** that combine a shader with concrete resource assignments.

### 7.1 Defining a material

```solid
material phong_red = material::create {
    shader: phong_shader,
    textures: {
        { binding: 0, texture: "assets/red_diffuse.png" },
    },
    uniforms: {
        { binding: 0, value: matrix4x4::identity() },
    },
    blend:    blend::opaque(),
    depth:    depth::less_equal(),
    raster:   raster::cull_back(),
};
```

### 7.2 Material instances

```solid
material base = material::load("materials/wood.json");

// Clone with override
material instance = material::clone(base);
material::set_texture(instance, 0, new_diffuse);
material::set_uniform(instance, "tint", float4(1, 0.5, 0.5, 1));

// Material variant system
material damaged = material::variant(base, "damaged", {
    { texture: 0, path: "assets/wood_damaged.png" },
});
```

### 7.3 Material overrides at draw call

```solid
draw(mesh, material) {
    // Default material
}

draw(mesh, material) with {
    uniforms: {
        { binding: 0, value: override_mvp },
    },
} {
    // Material with per-draw overrides
}
```

---

## 8. Scene Description

SolidScript can describe complete scenes:

```solid
@Solid

scene MainScene {
    // Camera
    camera main_cam = camera::perspective {
        fov:      60.0,
        near:     0.1,
        far:      1000.0,
        position: float3(0, 2, 5),
        target:   float3(0, 0, 0),
    };

    // Lights
    light sun = light::directional {
        color:     #FFF4E0,
        intensity: 3.0,
        direction: float3(0.5, -1, -0.3),
    };

    light fill = light::directional {
        color:     #4488FF,
        intensity: 0.5,
        direction: float3(-0.5, -0.3, 0.7),
    };

    // Materials
    material floor_mat = material::create {
        shader:   pbr_shader,
        textures: {
            { binding: 0, texture: "assets/floor_albedo.png" },
            { binding: 1, texture: "assets/floor_normal.png" },
            { binding: 2, texture: "assets/floor_roughness.png" },
            { binding: 3, texture: "assets/floor_metal.png" },
        },
    };

    // Entities
    entity floor = entity {
        mesh:     mesh::plane(20, 20),
        material: floor_mat,
        transform: {
            position: float3(0, -0.5, 0),
        },
    };

    entity teapot = entity {
        mesh:     mesh::load("assets/teapot.obj"),
        material: pbr_metal,
        transform: {
            position: float3(0, 0.5, 0),
            rotation: float3(0, 45_deg, 0),
            scale:    float3(0.5, 0.5, 0.5),
        },
    };

    // Post-processing
    postprocess tonemap = postprocess::hdr_tonemap {
        exposure: 1.0,
        method:   Reinhard,
    };

    postprocess bloom = postprocess::bloom {
        threshold: 1.2,
        radius:    0.05,
        intensity: 0.8,
    };
}
```

---

## 9. Integration with Vex

SolidScript is **not a separate language** — it is a Vex `@Macro` library. Every `.solid` file is processed by the standard Vex compiler pipeline with only one addition: the `@Solid` annotation macro.

### 9.1 What `@Solid` does

When VPP encounters `@Solid`, it:

1. **Imports the SolidScript standard library** (`<solid/std>`, `<solid/math>`, `<solid/pipeline>`, `<solid/types>`)
2. **Registers DSL types** → creates `typedef`/`struct` aliases:
   - `float3` → `SolidFloat3`
   - `matrix4x4` → `SolidMatrix4x4`
   - `texture` → `SolidTexture`
   - etc.
3. **Enables shader patterns** → `@vertex`, `@fragment`, `@compute` become Vex `@Macro` generators that produce the actual render pipeline FFI calls
4. **Processes uniform bindings** → `@binding(slot: N, stage: S) uniform T name;` becomes a Vex `extern` declaration with metadata
5. **Generates pipeline descriptor** → the shader combination is lowered to a `SolidPipeline` struct that VestaVM's graphics FFI layer consumes

### 9.2 Using SolidScript from plain Vex

```vex
// main.vex — pure Vex file using SolidScript modules
import solid.std;
import solid.math;

@SolidModule("shaders/triangle.solid")

f64 main() {
    // Scene setup via SolidScript-generated types
    SolidCamera cam = solid_camera_create_perspective(60.0, 16.0/9.0, 0.1, 100.0);
    SolidMesh tri = solid_mesh_create_triangle();
    SolidShader shader = solid_shader_load("shaders/triangle.solid");
    SolidMaterial mat = solid_material_create(shader);

    // Render loop
    while (solid_window_is_open()) {
        solid_poll_events();
        solid_begin_frame(cam);

        solid_draw(tri, mat);

        solid_end_frame();
        solid_present();
    }

    return 0;
}
```

### 9.3 Compile-time vs runtime

| Construct | When resolved | Details |
| :-------- | :------------ | :------ |
| Type aliases (`float3`, etc.) | Compile-time | `typedef` / `struct` expansion |
| Shader compilation | Compile-time | `@Solid` macro emits Vex code |
| Pipeline creation | Runtime | VestaVM FFI to Vulkan/Metal/DX12 |
| Texture loading | Runtime | Asset system via FFI |
| Material binding | Runtime | Pipeline + descriptor set creation |

### 9.4 Vex features available in SolidScript

SolidScript inherits all Vex features:

- **Static typing** with local inference
- **Generics** (`struct Light<T>`)
- **Smart pointers** (`unique<SolidMesh>`, `shared<SolidTexture>`)
- **Borrow checker** (`borrow<SolidPipeline>`)
- **Metaprogramming** (`@Macro` within shader bodies)
- **Async/await** for asset streaming
- **Pattern matching** (`match light.kind { ... }`)
- **FFI** to native graphics APIs
- **Compile-time FFI** for shader compilation

---

## 10. Standard Library

The SolidScript standard library (`<solid/std>`) provides:

### 10.1 Modules

| Module | Contents |
| :----- | :------- |
| `<solid/std>` | Core types, constants, pipeline descriptors |
| `<solid/math>` | Vector math, swizzle macros, matrix construction |
| `<solid/noise>` | Perlin, simplex, Worley noise functions |
| `<solid/pipeline>` | Pipeline creation, shader compilation, draw calls |
| `<solid/material>` | Material definition and instantiation |
| `<solid/light>` | Light types and attenuation functions |
| `<solid/mesh>` | Mesh creation, loading, generation (sphere, cube, plane) |
| `<solid/camera>` | Camera projection and view matrix generation |
| `<solid/scene>` | Scene graph, transform hierarchy, entity system |
| `<solid/postprocess>` | Full-screen effects: bloom, tonemap, DOF, SSAO |
| `<solid/shadow>` | Shadow mapping utilities (PCF, VSM, CSM) |

### 10.2 Constants

```solid
const f32 PI       = 3.14159265359;
const f32 TWO_PI   = 6.28318530718;
const f32 HALF_PI  = 1.57079632679;
const f32 EPSILON  = 1.19209290e-7;
const f32 INFINITY = 3.40282347e+38;
```
