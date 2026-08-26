import common;
import gpu;
import scene;
import particles;

#include <defines.slang>

struct PushConstants {
    u32 total_spawn;
    u32 emitter_count;
};
[[vk::push_constant]] PushConstants C;

[[vk::binding(0, 0)]] RWStructuredBuffer<Particle> particles;
[[vk::binding(1, 0)]] RWStructuredBuffer<u32> dead_list;
[[vk::binding(2, 0)]] RWStructuredBuffer<u32> dead_counts;
[[vk::binding(3, 0)]] RWStructuredBuffer<u32> alive_list;
[[vk::binding(4, 0)]] RWStructuredBuffer<ParticleCounters> counters;
[[vk::binding(5, 0)]] StructuredBuffer<ParticleEmitter> emitters;
[[vk::binding(6, 0)]] StructuredBuffer<ParticleInstruction> program;
[[vk::binding(7, 0)]] StructuredBuffer<f32x4> constants;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let index = thread_id.x;

    var emitter_index = 0u;
    var found = false;
    if (index < C.total_spawn) {
        for (u32 i = 0; i < C.emitter_count; i++) {
            let e = emitters[i];
            if (index >= e.spawn_offset && index < e.spawn_offset + e.spawn_count) {
                emitter_index = i;
                found = true;
                break;
            }
        }
    }

    var spawned = false;
    var particle_index = 0u;
    Particle particle = {};

    if (found) {
        let emitter = emitters[emitter_index];
        let local_index = index - emitter.spawn_offset;

        let previous = __atomic_add(dead_counts[emitter_index], ~0u, MemoryOrder::Relaxed);
        if (previous == 0 || previous > emitter.capacity) {
            __atomic_add(dead_counts[emitter_index], 1u, MemoryOrder::Relaxed);
        } else {
            spawned = true;
            particle_index = dead_list[emitter.pool_offset + previous - 1u];

            let rng = particle_hash(
                emitter.seed ^ particle_hash(particle_index * 2654435761u + u32(emitter.time * 1000.0) + local_index)
            );

            f32x3 direction;
            let local_position = particle_emission_point(emitter, rng, direction);

            let lifetime = lerp(emitter.lifetime.x, emitter.lifetime.y, particle_random(rng + 7u));

            particle.life_total = max(lifetime, 1e-4);
            particle.life_remaining = particle.life_total;
            particle.size = f32x2(1.0, 1.0);
            particle.rotation = 0.0;
            particle.seed = f32(rng & 0x00FFFFFFu) / f32(0x01000000u);
            particle.color = 0xFFFFFFFFu;
            particle.flipbook_frame = 0;
            particle.emitter_index = emitter_index;
            particle.flags = 1;

            if (HAS_FLAG(emitter.flags, ParticleEmitterFlags.LocalSpace)) {
                particle.position = local_position;
                particle.velocity = direction;
            } else {
                particle.position = mul(emitter.transform, f32x4(local_position, 1.0)).xyz;
                particle.velocity = particle_rotate_by_quat(emitter.rotation, direction);
            }

            var vm = ParticleVM(particle, emitter, 0.0);
            vm.rng = rng;
            particle_execute(
                vm,
                emitter,
                program,
                constants,
                emitter.spawn_program_offset,
                emitter.spawn_program_count
            );
            vm.store(particle);

            particles[particle_index] = particle;
        }
    }

    let lane_count = spawned ? 1u : 0u;
    let lane_offset = WavePrefixSum(lane_count);
    let wave_count = WaveActiveSum(lane_count);

    var base = 0u;
    if (WaveIsFirstLane() && wave_count > 0) {
        base = __atomic_add(counters[0].alive_count, wave_count, MemoryOrder::Relaxed);
    }
    base = WaveReadLaneFirst(base);

    if (spawned) {
        alive_list[base + lane_offset] = particle_index;
    }
}
