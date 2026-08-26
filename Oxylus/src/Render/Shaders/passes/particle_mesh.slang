import common;
import gpu;
import scene;
import particles;

#include <defines.slang>

struct PushConstants {
    Material* materials;
    Mesh* mesh;
    u32 emitter_index;
};
[[vk::push_constant]] PushConstants C;

[[vk::binding(0, 0)]] StructuredBuffer<ParticleSortKey> sort_keys;
[[vk::binding(1, 0)]] StructuredBuffer<Particle> particles;
[[vk::binding(2, 0)]] StructuredBuffer<ParticleEmitter> emitters;
[[vk::binding(3, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(4, 0)]] Image2D<f32> depth_image;

struct VOutput {
    f32x4 position : SV_Position;
    f32x2 uv;
    f32x3 normal;
    f32x4 color;
    f32 particle_depth;
    u32 material_index;
    u32 flags;
};

func particle_basis(in Particle particle) -> f32x3x3 {
    let c = cos(particle.rotation);
    let s = sin(particle.rotation);

    return f32x3x3(
        f32x3(c, 0.0, -s) * particle.size.x,
        f32x3(0.0, 1.0, 0.0) * particle.size.y,
        f32x3(s, 0.0, c) * particle.size.x
    );
}

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID, u32 instance_id : SV_InstanceID) -> VOutput {
    VOutput output = (VOutput)0;

    let particle = particles[sort_keys[instance_id].index];
    if (particle.emitter_index != C.emitter_index) {
        return output;
    }

    let emitter = emitters[particle.emitter_index];

    let local_position = C.mesh.decode_position(vertex_index);
    let local_normal = C.mesh.decode_normal(vertex_index);

    let basis = particle_basis(particle);
    var world = mul(local_position, basis) + particle.position;
    var normal = normalize(mul(local_normal, basis));

    if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.LocalSpace)) {
        world = mul(emitter.transform, f32x4(world, 1.0)).xyz;
        normal = normalize(mul(emitter.transform, f32x4(normal, 0.0)).xyz);
    }

    let clip = mul(camera.projection_view, f32x4(world, 1.0));

    output.position = clip;
    output.uv = C.mesh.texture_coords == nullptr ? f32x2(0.0) : C.mesh.decode_texcoord(vertex_index);
    output.normal = normal;
    output.color = particle_unpack_rgba8(particle.color);
    output.particle_depth = clip.w > 0.0 ? clip.z / clip.w : 0.0;
    output.material_index = emitter.material_index;
    output.flags = emitter.flags;

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VOutput input) : SV_Target0 {
    let pixel = i32x2(input.position.xy);
    let scene_depth = depth_image.Load(i32x3(pixel, 0)).r;

    if (input.particle_depth < scene_depth) {
        discard;
    }

    let material = C.materials[input.material_index];

    UVGradient grad;
    grad.uv = input.uv;
    grad.ddx = ddx(input.uv);
    grad.ddy = ddy(input.uv);

    var color = material.sample_albedo_color(grad) * input.color;

    let shade = saturate(dot(normalize(input.normal), f32x3(0.0, 1.0, 0.0)) * 0.5 + 0.5);
    color.rgb *= lerp(0.6, 1.0, shade);

    if (color.a <= 0.001) {
        discard;
    }

    color.rgb *= color.a;
    if (HAS_FLAG(input.flags, ParticleEmitterFlags.Additive)) {
        color.a = 0.0;
    }

    return color;
}
