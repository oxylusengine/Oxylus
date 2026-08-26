import common;
import gpu;
import particles;

struct PushConstants {
    u32 total_capacity;
};
[[vk::push_constant]] PushConstants C;

[[vk::binding(0, 0)]] RWStructuredBuffer<u32> dead_list;
[[vk::binding(1, 0)]] RWStructuredBuffer<Particle> particles;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let index = thread_id.x;
    if (index >= C.total_capacity) {
        return;
    }

    dead_list[index] = index;

    Particle particle = {};
    particles[index] = particle;
}
