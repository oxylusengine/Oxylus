import common;
import gpu;
import scene;
import particles;

#include <defines.slang>

struct PushConstants {
    Material* materials;
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
    f32x4 color;
    f32 particle_depth;
    f32 view_distance;
    f32 soft_distance;
    u32 material_index;
    u32 flags;
};

[[shader("vertex")]]
func vs_main(u32 vertex_id : SV_VertexID, u32 instance_id : SV_InstanceID) -> VOutput {
    VOutput output = (VOutput)0;

    let particle = particles[sort_keys[instance_id].index];
    let emitter = emitters[particle.emitter_index];
    if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.MeshRenderer)) {
        return output;
    }

    const f32x2 corners[6] = {
        f32x2(-0.5, -0.5),
        f32x2(0.5, -0.5),
        f32x2(0.5, 0.5),
        f32x2(0.5, 0.5),
        f32x2(-0.5, 0.5),
        f32x2(-0.5, -0.5),
    };
    const f32x2 uvs[6] = {
        f32x2(0.0, 1.0),
        f32x2(1.0, 1.0),
        f32x2(1.0, 0.0),
        f32x2(1.0, 0.0),
        f32x2(0.0, 0.0),
        f32x2(0.0, 1.0),
    };

    let corner = corners[vertex_id % 6];
    var uv = uvs[vertex_id % 6];

    var center = particle.position;
    if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.LocalSpace)) {
        center = mul(emitter.transform, f32x4(center, 1.0)).xyz;
    }

    const f32x3 world_up = f32x3(0.0, 1.0, 0.0);
    let c = cos(particle.rotation);
    let s = sin(particle.rotation);

    f32x3 axis_x;
    f32x3 axis_y;
    if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.VelocityStretched)) {
        var velocity = particle.velocity;
        if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.LocalSpace)) {
            velocity = mul(emitter.transform, f32x4(velocity, 0.0)).xyz;
        }

        let speed = length(velocity);
        axis_y = speed > 1e-4 ? velocity / speed : camera.up;
        axis_x = normalize(cross(axis_y, camera.forward) + f32x3(1e-6));
        axis_y *= 1.0 + speed * emitter.velocity_stretch;
    } else if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.HorizontalPlane)) {
        axis_x = f32x3(c, 0.0, -s);
        axis_y = f32x3(s, 0.0, c);
    } else if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.VerticalPlane)) {
        // rotation is dropped here on purpose: spinning the quad would tip it off the up axis, which
        // is the one thing this mode promises
        let side = cross(world_up, camera.position.xyz - center);
        let side_length = length(side);
        axis_x = side_length > 1e-4 ? side / side_length : camera.right;
        axis_y = world_up;
    } else {
        axis_x = camera.right * c + camera.up * s;
        axis_y = camera.up * c - camera.right * s;
    }

    let world = center + axis_x * (corner.x * particle.size.x) + axis_y * (corner.y * particle.size.y);
    let clip = camera.jitter_clip(mul(camera.projection_view, f32x4(world, 1.0)));

    let tiles = f32x2(f32(max(emitter.flipbook_x, 1u)), f32(max(emitter.flipbook_y, 1u)));
    let frame = f32(particle.flipbook_frame);
    let tile = f32x2(fmod(frame, tiles.x), floor(frame / tiles.x));
    uv = (uv + tile) / tiles;

    output.position = clip;
    output.uv = uv;
    output.color = particle_unpack_rgba8(particle.color);
    output.particle_depth = clip.w > 0.0 ? clip.z / clip.w : 0.0;
    output.view_distance = distance(camera.position.xyz, world);
    output.soft_distance = emitter.soft_particle_distance;
    output.material_index = emitter.material_index;
    output.flags = emitter.flags;

    return output;
}

// Alpha blended content has no usable motion vector, so instead of ghosting it is marked reactive
// and the upscaler leans on the current frame in those pixels. The SDK asks that reactivity stay at
// or below 0.9 so history is never fully discarded.
struct FragmentOutput {
    f32x4 color : SV_Target0;
    f32 reactive : SV_Target1;
};

[[shader("fragment")]]
FragmentOutput fs_main(VOutput input) {
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

    if (HAS_FLAG(input.flags, ParticleEmitterFlags.SoftParticles) && input.soft_distance > 0.0) {
        let surface = camera.unproject_uv(input.position.xy / max(camera.resolution, f32x2(1.0)), scene_depth);
        let separation = distance(surface, camera.position.xyz) - input.view_distance;
        color.a *= saturate(separation / input.soft_distance);
    }

    if (color.a <= 0.001) {
        discard;
    }

    // captured before the additive path zeroes alpha, so additive particles still register
    let coverage = color.a;

    color.rgb *= color.a;
    if (HAS_FLAG(input.flags, ParticleEmitterFlags.Additive)) {
        color.a = 0.0;
    }

    FragmentOutput output;
    output.color = color;
    output.reactive = min(coverage, 0.9);

    return output;
}
