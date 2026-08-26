import common;
import gpu;
import particles;

struct PushConstants {
    u32 mesh_draw_count;
};
[[vk::push_constant]] PushConstants C;

[[vk::binding(0, 0)]] StructuredBuffer<ParticleCounters> counters;
[[vk::binding(1, 0)]] RWStructuredBuffer<DrawIndirectCommand> draw_args;
[[vk::binding(2, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> draw_indexed_args;
[[vk::binding(3, 0)]] StructuredBuffer<u32> mesh_index_counts;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let alive = counters[0].alive_count_next;

    if (thread_id.x == 0) {
        draw_args[0].vertex_count = 6;
        draw_args[0].instance_count = alive;
        draw_args[0].first_vertex = 0;
        draw_args[0].first_instance = 0;
    }

    if (thread_id.x < C.mesh_draw_count) {
        draw_indexed_args[thread_id.x].index_count = mesh_index_counts[thread_id.x];
        draw_indexed_args[thread_id.x].instance_count = alive;
        draw_indexed_args[thread_id.x].first_index = 0;
        draw_indexed_args[thread_id.x].vertex_offset = 0;
        draw_indexed_args[thread_id.x].first_instance = 0;
    }
}
