# SolidScript — Examples

---

## Example 1: Simple Vertex Shader — Rotating Triangle

File: `examples/triangle.solid`

```solid
@Solid

#import <solid/std>
#import <solid/math>

// ====== Vertex input ======
struct VertexInput {
    @input(binding: 0) float3 position;
    @input(binding: 1) float3 color;
}

// ====== Inter-stage data ======
struct VSOutput {
    @builtin(SV_Position) float4 position;
    float3 color;
}

// ====== Uniforms ======
@binding(slot: 0, stage: Vertex)
uniform matrix4x4 modelViewProj;

// ====== Vertex shader ======
@vertex
VSOutput vs_main(VertexInput input) {
    VSOutput out;
    out.position = modelViewProj * float4(input.position, 1.0);
    out.color    = input.color;
    return out;
}

// ====== Fragment shader ======
@fragment
float4 fs_main(VSOutput input) {
    return float4(input.color, 1.0);
}
```

### Host code (plain Vex)

File: `main.vex`

```vex
import solid.std;

@SolidModule("triangle.solid")

f64 main() {
    // Create window
    SolidWindow window = solid_window_create(800, 600, "Rotating Triangle");
    if (window == null) return 1;

    // Load shader
    SolidShader shader = solid_shader_load("triangle.solid");

    // Vertex data: positions + colors
    unique<float3[]> positions = new float3[3] {
        float3( 0.0,  0.5, 0.0),
        float3(-0.5, -0.5, 0.0),
        float3( 0.5, -0.5, 0.0),
    };

    unique<float3[]> colors = new float3[3] {
        float3(1.0, 0.0, 0.0),
        float3(0.0, 1.0, 0.0),
        float3(0.0, 0.0, 1.0),
    };

    // Create mesh
    SolidMesh mesh = solid_mesh_create(positions, null, colors);

    // Create material
    SolidMaterial mat = solid_material_create(shader);

    // Camera
    SolidCamera cam = solid_camera_perspective(60.0, 800.0/600.0, 0.1, 100.0);
    solid_camera_set_position(cam, float3(0, 0, 2));
    solid_camera_look_at(cam, float3(0, 0, 0));

    f32 angle = 0.0;

    // Render loop
    while (solid_window_is_open(window)) {
        solid_poll_events();

        angle += 0.01;
        matrix4x4 rot = matrix4x4::rotate_y(angle);
        matrix4x4 mvp = solid_camera_view_proj(cam) * rot;

        solid_material_set_uniform(mat, 0, mvp);

        solid_begin_frame(window, cam);
        solid_draw(mesh, mat);
        solid_end_frame();
        solid_present(window);
    }

    solid_window_destroy(window);
    return 0;
}
```

---

## Example 2: Fragment Shader with Phong Lighting

File: `examples/phong.solid`

```solid
@Solid

#import <solid/std>
#import <solid/math>

// ====== Vertex input ======
struct VertexInput {
    @input(binding: 0) float3 position;
    @input(binding: 1) float3 normal;
    @input(binding: 2) float2 uv;
}

// ====== VS Output / FS Input ======
struct VSOutput {
    @builtin(SV_Position) float4 position;
    float3 world_pos;
    float3 normal;
    float2 uv;
    float3 view_dir;
}

// ====== Uniforms ======
@binding(slot: 0, stage: Vertex)
uniform matrix4x4 modelViewProj;

@binding(slot: 1, stage: Vertex)
uniform matrix4x4 model;

@binding(slot: 2, stage: Vertex)
uniform matrix4x4 normalMatrix;  // transpose(inverse(model))

@binding(slot: 3, stage: Fragment)
uniform struct LightParams {
    float3  direction;
    float3  ambient;
    float3  diffuse;
    float3  specular;
    float3  eye_pos;
    f32     shininess;
} light;

@binding(slot: 4, stage: Fragment)
uniform texture diffuseTex;

@binding(slot: 5, stage: Fragment)
uniform sampler linearSampler;

// ====== Vertex shader ======
@vertex
VSOutput vs_main(VertexInput input) {
    VSOutput out;

    float4 world = model * float4(input.position, 1.0);
    out.position  = modelViewProj * float4(input.position, 1.0);
    out.world_pos = world.xyz;
    out.normal    = normalize((normalMatrix * float4(input.normal, 0.0)).xyz);
    out.uv        = input.uv;
    out.view_dir  = normalize(light.eye_pos - world.xyz);

    return out;
}

// ====== Fragment shader ======
@fragment
float4 fs_main(VSOutput input) {
    float3 N = normalize(input.normal);
    float3 L = normalize(light.direction);
    float3 V = normalize(input.view_dir);
    float3 H = normalize(L + V);

    // Texture
    float4 tex_color = texture_sample(diffuseTex, linearSampler, input.uv);

    // Ambient
    float3 ambient = light.ambient * tex_color.rgb;

    // Diffuse
    f32 NdotL = max(dot(N, L), 0.0);
    float3 diffuse = NdotL * light.diffuse * tex_color.rgb;

    // Specular (Blinn-Phong)
    f32 NdotH = max(dot(N, H), 0.0);
    f32 spec = pow(NdotH, light.shininess);
    float3 specular = spec * light.specular;

    // Attenuation (no falloff for directional)
    float3 final = ambient + diffuse + specular;

    return float4(final, tex_color.a);
}
```

---

## Example 3: Compute Shader — Particle System

File: `examples/particles.solid`

```solid
@Solid

#import <solid/std>
#import <solid/math>

// ====== Particle data ======
struct Particle {
    float3 position;
    float3 velocity;
    float4 color;
    f32    life;
    f32    max_life;
    f32    size;
}

// ====== Storage buffers ======
@binding(slot: 0, stage: Compute)
@storage(readwrite)
uniform ParticleBuffer {
    Particle particles[];
} particle_buf;

@binding(slot: 1, stage: Compute)
uniform struct SimParams {
    f32    delta_time;
    f32    gravity;
    f32    ground_y;
    f32    emitter_rate;
    float3 emitter_pos;
    float3 wind;
    u32    particle_count;
    u32    alive_count;
} params;

// ====== Compute shader ======
@compute(local_size_x: 64, local_size_y: 1, local_size_z: 1)
void cs_main(@builtin(SV_GlobalInvocationID) uint3 id) {
    u32 index = id.x;
    if (index >= params.particle_count) return;

    Particle p = particle_buf.particles[index];

    // Skip dead particles
    if (p.life <= 0.0) return;

    // Update life
    p.life -= params.delta_time;

    // Physics
    p.velocity += float3(0.0, params.gravity, 0.0) * params.delta_time;
    p.velocity += params.wind * params.delta_time;
    p.position += p.velocity * params.delta_time;

    // Ground collision
    if (p.position.y < params.ground_y) {
        p.position.y = params.ground_y;
        p.velocity = float3(
            p.velocity.x * 0.8,
            -p.velocity.y * 0.3,
            p.velocity.z * 0.8
        );
    }

    // Fade out
    f32 life_ratio = p.life / p.max_life;
    p.color.a = saturate(life_ratio);

    // Size grows then shrinks
    f32 size_curve = 1.0 - abs(life_ratio - 0.5) * 2.0;
    p.size = 0.1 + size_curve * 0.4;

    particle_buf.particles[index] = p;
}
```

### Host compute dispatch (Vex)

```vex
import solid.std;
import solid.pipeline;

@SolidModule("particles.solid")

const u32 PARTICLE_COUNT = 65536;

f64 main() {
    SolidWindow window = solid_window_create(1280, 720, "Particles");
    SolidComputePipeline pipeline = solid_compute_pipeline_create("particles.solid");

    // Initialize particle buffer
    unique<Particle[]> particles = new Particle[PARTICLE_COUNT];
    for (u32 i = 0; i < PARTICLE_COUNT; i++) {
        particles[i] = Particle {
            position:  float3(0, 2, 0),
            velocity:  float3(random_float(-1, 1), random_float(2, 5), random_float(-1, 1)),
            color:     float4(1, 0.5, 0, 1),
            life:      random_float(1.0, 3.0),
            max_life:  3.0,
            size:      0.2,
        };
    }

    SolidBuffer particle_gpu = solid_buffer_create_gpu(
        particles, PARTICLE_COUNT * sizeof(Particle),
        BufferUsage::Storage | BufferUsage::Vertex
    );

    // Render mesh for each particle (point sprites)
    SolidMesh point_mesh = solid_mesh_create_quad();

    // Simulation parameters (will be updated per frame)
    SimParams sim_params = SimParams {
        delta_time:     0.016,
        gravity:        -9.81,
        ground_y:       0.0,
        emitter_rate:   100.0,
        emitter_pos:    float3(0, 3, 0),
        wind:           float3(2, 0, 1),
        particle_count: PARTICLE_COUNT,
        alive_count:    PARTICLE_COUNT,
    };

    SolidBuffer params_gpu = solid_buffer_create_gpu(
        &sim_params, sizeof(SimParams),
        BufferUsage::Uniform
    );

    // Render loop
    while (solid_window_is_open(window)) {
        solid_poll_events();

        f32 dt = solid_delta_time();
        sim_params.delta_time = dt;
        solid_buffer_upload(params_gpu, &sim_params, sizeof(SimParams));

        // Dispatch compute
        solid_compute_dispatch(pipeline, PARTICLE_COUNT / 64, 1, 1);

        // Render particles as instanced quads
        solid_begin_frame(window, camera);
        solid_draw_instanced(point_mesh, particle_material, PARTICLE_COUNT);
        solid_end_frame();
        solid_present(window);
    }

    return 0;
}
```

---

## Example 4: Material Definition with Textures

File: `examples/pbr_material.solid`

```solid
@Solid

#import <solid/std>
#import <solid/math>

// ====== PBR Shader ======

struct VertexInput {
    @input(binding: 0) float3 position;
    @input(binding: 1) float3 normal;
    @input(binding: 2) float4 tangent;
    @input(binding: 3) float2 uv;
}

struct VSOutput {
    @builtin(SV_Position) float4 position;
    float3 world_pos;
    float3 normal;
    float4 tangent;
    float2 uv;
    float3 view_dir;
}

@binding(slot: 0, stage: Vertex)
uniform matrix4x4 model;

@binding(slot: 1, stage: Vertex)
uniform matrix4x4 viewProj;

@binding(slot: 2, stage: Vertex)
uniform matrix4x4 normalMatrix;

@binding(slot: 3, stage: Fragment)
uniform float3 eye_pos;

// PBR material parameters
@binding(slot: 4, stage: Fragment)
uniform texture albedoTex;

@binding(slot: 5, stage: Fragment)
uniform texture normalTex;

@binding(slot: 6, stage: Fragment)
uniform texture metallicTex;

@binding(slot: 7, stage: Fragment)
uniform texture roughnessTex;

@binding(slot: 8, stage: Fragment)
uniform texture aoTex;

@binding(slot: 9, stage: Fragment)
uniform sampler linearSampler;

@binding(slot: 10, stage: Fragment)
uniform sampler pointSampler;

// PBR constants (optional override)
@binding(slot: 11, stage: Fragment)
uniform struct PBRParams {
    float4 base_color;
    f32    metallic;
    f32    roughness;
    f32    ao;
} pbr_params;

// Lights (directional + 4 point lights)
@binding(slot: 12, stage: Fragment)
uniform struct DirectionalLight {
    float3 direction;
    float3 color;
    f32    intensity;
} dir_light;

@binding(slot: 13, stage: Fragment)
uniform struct PointLights {
    float3 positions[4];
    float3 colors[4];
    f32    ranges[4];
} point_lights;

@binding(slot: 14, stage: Fragment)
uniform texture brdfLUT;

// ====== Vertex shader ======
@vertex
VSOutput vs_main(VertexInput input) {
    VSOutput out;

    float4 world = model * float4(input.position, 1.0);
    out.position  = viewProj * float4(input.position, 1.0);
    out.world_pos = world.xyz;
    out.normal    = normalize((normalMatrix * float4(input.normal, 0.0)).xyz);
    out.tangent   = float4(
        normalize((model * float4(input.tangent.xyz, 0.0)).xyz),
        input.tangent.w
    );
    out.uv        = input.uv;
    out.view_dir  = normalize(eye_pos - world.xyz);

    return out;
}

// ====== Fragment shader (Cook-Torrance BRDF) ======
@fragment
float4 fs_main(VSOutput input) {
    // Sample textures
    float4 albedo     = texture_sample(albedoTex, linearSampler, input.uv) * pbr_params.base_color;
    float3 normal_map = texture_sample(normalTex, linearSampler, input.uv).rgb;
    f32 metallic      = texture_sample(metallicTex,   pointSampler, input.uv).r * pbr_params.metallic;
    f32 roughness     = texture_sample(roughnessTex,  pointSampler, input.uv).r * pbr_params.roughness;
    f32 ao            = texture_sample(aoTex,         pointSampler, input.uv).r * pbr_params.ao;

    // Tangent-space normal
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent.xyz);
    float3 B = cross(N, T) * input.tangent.w;
    float3x3 TBN = { T, B, N };
    normal_map = normal_map * 2.0 - 1.0;
    normal_map = normalize(TBN * normal_map);
    N = normal_map;

    float3 V = normalize(input.view_dir);
    float3 F0 = mix(float3(0.04), albedo.rgb, metallic);

    // Lighting accumulation
    float3 Lo = float3(0.0);

    // Directional light
    {
        float3 L = normalize(-dir_light.direction);
        float3 H = normalize(L + V);
        f32 NdotV = max(dot(N, V), EPSILON);
        f32 NdotL = max(dot(N, L), EPSILON);
        f32 HdotV = max(dot(H, V), EPSILON);
        f32 NdotH = max(dot(N, H), EPSILON);

        f32 NDF = distribution_ggx(NdotH, roughness);
        f32 G   = geometry_smith(NdotV, NdotL, roughness);
        float3 F = fresnel_schlick(HdotV, F0);

        float3 kS = F;
        float3 kD = (float3(1.0) - kS) * (1.0 - metallic);
        float3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, EPSILON);

        Lo += (kD * albedo.rgb / PI + specular) * dir_light.color * dir_light.intensity * NdotL;
    }

    // Point lights
    for (i32 i = 0; i < 4; i++) {
        float3 pos = point_lights.positions[i];
        float3 col = point_lights.colors[i];
        f32 range   = point_lights.ranges[i];

        float3 L = pos - input.world_pos;
        f32 dist = length(L);
        L = L / dist;

        f32 attenuation = 1.0 / (dist * dist + 1.0);

        float3 H = normalize(L + V);
        f32 NdotV = max(dot(N, V), EPSILON);
        f32 NdotL = max(dot(N, L), EPSILON);
        f32 HdotV = max(dot(H, V), EPSILON);
        f32 NdotH = max(dot(N, H), EPSILON);

        f32 NDF = distribution_ggx(NdotH, roughness);
        f32 G   = geometry_smith(NdotV, NdotL, roughness);
        float3 F = fresnel_schlick(HdotV, F0);

        float3 kS = F;
        float3 kD = (float3(1.0) - kS) * (1.0 - metallic);
        float3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, EPSILON);

        Lo += (kD * albedo.rgb / PI + specular) * col * attenuation * NdotL;
    }

    // Ambient
    float3 ambient = float3(0.03) * albedo.rgb * ao;

    // Tonemap (ACES)
    float3 color = ambient + Lo;
    color = tonemap_aces(color);

    // Gamma correct
    color = pow(color, float3(1.0 / 2.2));

    return float4(color, albedo.a);
}

// ====== BRDF helper functions ======
f32 distribution_ggx(f32 NdotH, f32 roughness) {
    f32 a  = roughness * roughness;
    f32 a2 = a * a;
    f32 d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, EPSILON);
}

f32 geometry_schlick_ggx(f32 NdotV, f32 roughness) {
    f32 r = roughness + 1.0;
    f32 k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, EPSILON);
}

f32 geometry_smith(f32 NdotV, f32 NdotL, f32 roughness) {
    return geometry_schlick_ggx(NdotV, roughness)
         * geometry_schlick_ggx(NdotL, roughness);
}

float3 fresnel_schlick(f32 HdotV, float3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
}

float3 tonemap_aces(float3 x) {
    // ACES filmic (Narkowicz 2015 fit)
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}
```

---

## Example 5: Full SolidEngine Scene Setup

File: `examples/scene.solid`

```solid
@Solid

// Scene: Sponza atrium with animated lights and post-processing

#import <solid/std>
#import <solid/math>
#import <solid/scene>
#import <solid/postprocess>
#import <solid/shadow>

// ====== Scene entity types ======

struct Transform {
    float3 position;
    float3 rotation;
    float3 scale;
}

entity "Sponza" {
    mesh:     mesh::load("assets/sponza/sponza.obj"),
    material: material::load("assets/sponza/materials.json"),
    transform: {
        position: float3(0, 0, 0),
        rotation: float3(0, 0, 0),
        scale:    float3(1, 1, 1),
    },
}

// ====== Animated light ======
@Macro
comptime string animated_light(i32 index) {
    return `
        entity "light_${index}" {
            light: light::point {
                color:     #FF6600,
                intensity: 5.0 + 3.0 * sin(frame * 0.02 + ${index} * 2.094),
                position:  float3(
                    3.0 * cos(frame * 0.01 + ${index} * 2.094),
                    2.0,
                    3.0 * sin(frame * 0.01 + ${index} * 2.094)
                ),
                range:     8.0,
                shadows:   true,
            },
            transform: { },
        }
    `;
}

// Generate 3 animated point lights
animated_light(0);
animated_light(1);
animated_light(2);

// ====== Cascaded shadow map setup ======
shadow_map sun_shadows = shadow::cascaded {
    resolution:  4096,
    cascade_count: 4,
    split_lambda: 0.95,
    bias:        0.001,
    normal_bias: 0.01,
    filter:      PCF(4),
};

// ====== Camera ======
camera main_cam = camera::perspective {
    fov:       70.0,
    near:      0.1,
    far:       500.0,
    position:  float3(-5, 3, 0),
    target:    float3(0, 1.5, 0),
};

// ====== Skybox ======
skybox environment = skybox::load_cubemap {
    pos_x: "assets/sky/right.jpg",
    neg_x: "assets/sky/left.jpg",
    pos_y: "assets/sky/top.jpg",
    neg_y: "assets/sky/bottom.jpg",
    pos_z: "assets/sky/front.jpg",
    neg_z: "assets/sky/back.jpg",
};

// ====== Fog ======
fog scene_fog = fog::exponential {
    color:  float3(0.7, 0.75, 0.8),
    density: 0.008,
};

// ====== Post-processing stack ======
postprocess_stack effects = {
    bloom: bloom {
        threshold: 0.8,
        radius:    0.04,
        intensity: 0.6,
    },
    ssao: ssao {
        radius:     0.5,
        bias:       0.025,
        intensity:  1.5,
        samples:    16,
    },
    tonemap: tonemap::aces(),
    anti_alias: fxaa {
        edge_threshold: 0.166,
        edge_threshold_min: 0.083,
    },
};

// ====== Render pipeline overrides ======
render_pipeline main_pipeline {
    clear_color:  #1a1a2e,
    shadow_map:   sun_shadows,
    skybox:       environment,
    fog:          scene_fog,
    postprocess:  effects,

    // Per-viewport render layers
    layers: {
        opaque: {
            sort:    front_to_back,
            cull:    back,
            shaders: [pbr_shader, simple_shader],
        },
        transparent: {
            sort:    back_to_front,
            cull:    none,
            shaders: [glass_shader, particle_shader],
        },
        ui: {
            sort:    none,
            cull:    none,
            shaders: [ui_shader],
        },
    },
};
```

### Host loop with camera controls

File: `main.vex`

```vex
import solid.std;
import solid.scene;

@SolidModule("scene.solid")

f64 main() {
    SolidWindow window = solid_window_create(1920, 1080, "SolidEngine — Sponza");
    solid_window_set_fullscreen(window, true);

    SolidScene scene = solid_scene_load("scene.solid");

    // Camera orbit controls
    f32 pitch = 20.0;
    f32 yaw   = -45.0;
    f32 dist  = 8.0;

    while (solid_window_is_open(window)) {
        solid_poll_events();

        // Mouse orbit
        f32 dx = solid_mouse_delta_x();
        f32 dy = solid_mouse_delta_y();

        if (solid_mouse_button(MouseRight)) {
            yaw   += dx * 0.003;
            pitch += dy * 0.003;
            pitch  = clamp(pitch, -89.0, 89.0);
            dist  -= solid_mouse_scroll() * 0.5;
            dist   = clamp(dist, 1.0, 50.0);
        }

        // Update camera
        f32 rad_pitch = pitch * PI / 180.0;
        f32 rad_yaw   = yaw * PI / 180.0;

        SolidCamera cam = solid_scene_get_camera(scene, "main_cam");
        solid_camera_set_position(cam, float3(
            dist * cos(rad_pitch) * sin(rad_yaw),
            dist * sin(rad_pitch),
            dist * cos(rad_pitch) * cos(rad_yaw)
        ));
        solid_camera_look_at(cam, float3(0, 1.5, 0));

        // Render
        solid_scene_update(scene, solid_delta_time());
        solid_begin_frame(window);
        solid_scene_render(scene);
        solid_end_frame();
        solid_present(window);
    }

    return 0;
}
```

---

## Example 6: Ray Tracing Shader (Extension)

File: `examples/raytracer.solid`

```solid
@Solid

// Ray tracing: uses the @raygen/@miss/@closesthit extensions
// Requires: VestaVM with RTX backend

#import <solid/std>
#import <solid/math>
#import <solid/rt>

// ====== Ray generation shader ======
@raygen
void rgen_main() {
    float2 uv = float2(
        f32(launch_index.x) / f32(launch_size.x),
        f32(launch_index.y) / f32(launch_size.y)
    );

    // Camera ray
    float3 origin = eye_pos;
    float3 direction = normalize(uv.x * right + uv.y * up + forward);

    // Trace
    RayDesc ray = ray_desc(origin, EPSILON, direction, INFINITY);
    RayPayload payload { color: float3(0), depth: 0.0, hit: false };

    trace_ray(accel_struct, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    float3 color = payload.hit ? payload.color : sky_color(direction);
    image[launch_index] = float4(tonemap_aces(color), 1.0);
}

// ====== Closest-hit shader ======
@closesthit
void chs_main(inout RayPayload payload, Attributes attr) {
    float3 N = normalize(attr.normal);
    float3 V = normalize(-attr.ray_dir);
    float3 L = normalize(dir_light.direction);

    // Simple diffuse
    f32 NdotL = max(dot(N, L), 0.0);
    payload.color = tex_color.rgb * (0.05 + 0.95 * NdotL);
    payload.depth = attr.hit_t;
    payload.hit   = true;
}

// ====== Miss shader ======
@miss
void miss_main(inout RayPayload payload) {
    payload.color = sky_gradient(attr.ray_dir);
    payload.hit   = false;
}
```
