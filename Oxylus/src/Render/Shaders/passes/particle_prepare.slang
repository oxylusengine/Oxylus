import common;
import gpu;
import particles;

[[vk::binding(0, 0)]] RWStructuredBuffer<ParticleCounters> counters;

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main() -> void {
    counters[0].alive_count = counters[0].alive_count_next;
    counters[0].alive_count_next = 0;
    counters[0].draw_count = 0;
}
