import common;
import gpu;
import particles;

[[vk::binding(0, 0)]] StructuredBuffer<ParticleCounters> counters;
[[vk::binding(1, 0)]] RWStructuredBuffer<DispatchIndirectCommand> simulate_args;

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main() -> void {
    let alive = counters[0].alive_count;

    simulate_args[0].x = (alive + PARTICLE_SIMULATE_GROUP - 1) / PARTICLE_SIMULATE_GROUP;
    simulate_args[0].y = 1;
    simulate_args[0].z = 1;
}
