import common;
import gpu;
import scene;
import particles;

#include <defines.slang>

[[vk::binding(0, 0)]] RWStructuredBuffer<Particle> particles;
[[vk::binding(1, 0)]] StructuredBuffer<u32> alive_list_in;
[[vk::binding(2, 0)]] RWStructuredBuffer<u32> alive_list_out;
[[vk::binding(3, 0)]] RWStructuredBuffer<u32> dead_list;
[[vk::binding(4, 0)]] RWStructuredBuffer<u32> dead_counts;
[[vk::binding(5, 0)]] RWStructuredBuffer<ParticleCounters> counters;
[[vk::binding(6, 0)]] StructuredBuffer<ParticleEmitter> emitters;
[[vk::binding(7, 0)]] StructuredBuffer<ParticleInstruction> program;
[[vk::binding(8, 0)]] StructuredBuffer<f32x4> constants;
[[vk::binding(9, 0)]] RWStructuredBuffer<ParticleSortKey> sort_keys;
[[vk::binding(10, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(11, 0)]] Image2D<f32> depth_image;

func reconstruct_world(f32x2 uv, f32 depth) -> f32x3 {
    let ndc = f32x4(uv * 2.0 - 1.0, depth, 1.0);
    let world = mul(camera.inv_projection_view, ndc);
    return world.xyz / world.w;
}

func apply_depth_collision(inout Particle particle, in ParticleEmitter emitter, f32 delta_time) -> void {
    let clip = mul(camera.projection_view, f32x4(particle.position, 1.0));
    if (clip.w <= 0.0) {
        return;
    }

    let ndc = clip.xyz / clip.w;
    let uv = ndc.xy * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) {
        return;
    }

    let resolution = max(camera.resolution, f32x2(1.0));
    let pixel = i32x2(uv * resolution);
    let scene_depth = depth_image.Load(i32x3(pixel, 0)).r;
    if (scene_depth <= 0.0 || ndc.z >= scene_depth) {
        return;
    }

    let surface = reconstruct_world(uv, scene_depth);
    let travel = length(particle.velocity) * delta_time + 0.05;
    if (distance(surface, particle.position) > travel) {
        return;
    }

    let texel = 1.0 / resolution;
    let dx = reconstruct_world(uv + f32x2(texel.x, 0.0), depth_image.Load(i32x3(pixel + i32x2(1, 0), 0)).r);
    let dy = reconstruct_world(uv + f32x2(0.0, texel.y), depth_image.Load(i32x3(pixel + i32x2(0, 1), 0)).r);

    var normal = cross(dx - surface, dy - surface);
    if (dot(normal, normal) < 1e-12) {
        return;
    }

    normal = normalize(normal);
    if (dot(normal, particle.velocity) > 0.0) {
        normal = -normal;
    }

    particle.position = surface + normal * 0.01;
    particle.velocity = reflect(particle.velocity, normal) * emitter.restitution;
}

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let index = thread_id.x;
    let valid = index < counters[0].alive_count;

    var particle_index = 0u;
    var particle = (Particle)0;
    var emitter_index = 0u;
    var alive = false;

    if (valid) {
        particle_index = alive_list_in[index];
        particle = particles[particle_index];
        emitter_index = particle.emitter_index;

        let emitter = emitters[emitter_index];
        let delta_time = emitter.delta_time;

        particle.life_remaining -= delta_time;
        alive = particle.life_remaining > 0.0;

        if (alive) {
            let age = saturate(1.0 - particle.life_remaining / max(particle.life_total, 1e-5));

            var vm = ParticleVM(particle, emitter, age);
            particle_execute(
                vm,
                emitter,
                program,
                constants,
                emitter.update_program_offset,
                emitter.update_program_count
            );
            vm.store(particle);

            particle.position += particle.velocity * delta_time;

            let frame_count = max(emitter.flipbook_x * emitter.flipbook_y, 1u);
            particle.flipbook_frame = min(u32(age * f32(frame_count)), frame_count - 1u);

            if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.DepthCollision)) {
                apply_depth_collision(particle, emitter, delta_time);
            }
        }

        particles[particle_index] = particle;
    }

    let lane_count = alive ? 1u : 0u;
    let lane_offset = WavePrefixSum(lane_count);
    let wave_count = WaveActiveSum(lane_count);

    var base = 0u;
    if (WaveIsFirstLane() && wave_count > 0) {
        base = __atomic_add(counters[0].alive_count_next, wave_count, MemoryOrder::Relaxed);
    }
    base = WaveReadLaneFirst(base);

    if (alive) {
        let slot = base + lane_offset;
        alive_list_out[slot] = particle_index;

        let view_distance = distance(camera.position.xyz, particle.position);
        ParticleSortKey key;
        key.key = min(~asuint(view_distance), 0xFFFFFFFEu);
        key.index = particle_index;
        sort_keys[slot] = key;
    }

    if (valid && !alive) {
        let emitter = emitters[emitter_index];
        let slot = __atomic_add(dead_counts[emitter_index], 1u, MemoryOrder::Relaxed);
        dead_list[emitter.pool_offset + min(slot, emitter.capacity - 1u)] = particle_index;
    }
}
