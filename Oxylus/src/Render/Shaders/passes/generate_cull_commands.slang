import common;
import gpu;

[[vk::binding(0)]] StructuredBuffer<u32> visible_meshlet_instances_count;
[[vk::binding(1)]] RWStructuredBuffer<DispatchIndirectCommand> cull_meshlets_cmd;

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main() -> void {
    cull_meshlets_cmd[0].x = (visible_meshlet_instances_count[0] + (CULLING_MESHLET_COUNT - 1)) / CULLING_MESHLET_COUNT;
}
