import common;
import gpu;
import scene;
import cull;
import rmvsm;

#include <defines.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(2)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(4)]] StructuredBuffer<u32> layer_dirty_mask;
[[vk::binding(5)]] RWStructuredBuffer<VSMMeshletInstance> vsm_meshlet_instances;
[[vk::binding(6)]] RWStructuredBuffer<u32> vsm_meshlet_instance_count;
[[vk::binding(7)]] RWStructuredBuffer<DispatchIndirectCommand> cull_meshlets_cmd;

[[shader("compute")]]
[[numthreads(CULLING_MESH_COUNT, 1, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform VSMPointSpotContext ctx
) -> void {
    let mesh_instance_index = thread_id.x;
    let layer = thread_id.y;

    var meshlet_count = 0u;

    if (mesh_instance_index < ctx.mesh_instance_count && layer < ctx.layer_count) {
        let view = views[layer];
        let layer_has_dirty_pages = (layer_dirty_mask[layer] & (1u << u32(ctx.curr_mip))) != 0u;

        if (view.range > 0.0 && layer_has_dirty_pages) {
            let mesh_instance = mesh_instances[mesh_instance_index];
            let mesh = meshes[mesh_instance.mesh_index];
            let transform = transforms[mesh_instance.transform_index];

            let world_center = transform.to_world_position(mesh.bounds.aabb_center).xyz;
            let world_radius = transform.to_world_radius(length(mesh.bounds.aabb_extent * 0.5));
            let in_range = distance(world_center, view.light_position) - world_radius <= view.range;

            let mvp = mul(view.projection_view, transform.world);
            if (in_range && test_frustum(mvp, mesh.bounds.aabb_center, mesh.bounds.aabb_extent)) {
                meshlet_count = mesh.lods[mesh_instance.lod_index].meshlet_count;
            }
        }
    }

    let lane_meshlet_offset = WavePrefixSum(meshlet_count);
    let total_meshlet_count = WaveActiveSum(meshlet_count);

    var base = 0u;
    if (WaveIsFirstLane()) {
        base = __atomic_add(vsm_meshlet_instance_count[0], total_meshlet_count, MemoryOrder::Relaxed);
        let new_total = min(base + total_meshlet_count, VSM_POINT_SPOT_MAX_MESHLET_INSTANCES);
        let needed_groups = (new_total + CULLING_MESHLET_COUNT - 1u) / CULLING_MESHLET_COUNT;
        __atomic_max(cull_meshlets_cmd[0].x, needed_groups, MemoryOrder::Relaxed);
    }
    base = WaveReadLaneFirst(base);

    if (meshlet_count > 0u) {
        let base_offset = base + lane_meshlet_offset;
        for (u32 i = 0u; i < meshlet_count; i++) {
            let offset = base_offset + i;
            if (offset >= VSM_POINT_SPOT_MAX_MESHLET_INSTANCES) {
                break;
            }
            vsm_meshlet_instances[offset].mesh_instance_index = mesh_instance_index;
            vsm_meshlet_instances[offset].meshlet_index = i;
            vsm_meshlet_instances[offset].layer = layer;
        }
    }
}
