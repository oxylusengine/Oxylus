import common;
import gpu;
import scene;
import cull;

#include <defines.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<Transform> transforms;
[[vk::binding(2)]] RWStructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] RWStructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(4)]] RWStructuredBuffer<u32> visibility;
[[vk::binding(5)]] RWStructuredBuffer<DispatchIndirectCommand> cull_meshlets_cmd;

[[shader("compute")]]
[[numthreads(CULLING_MESH_COUNT, 1, 1)]]
func cs_main(
    uint3 thread_id : SV_DispatchThreadID,
    uniform f32x4x4 projection_view,
    uniform u32 mesh_instances_count,
) -> void {
    let mesh_instance_index = thread_id.x;

    var meshlet_count = 0u;
    var lod_index = 0u;

    if (mesh_instance_index < mesh_instances_count) {
        let mesh_instance = mesh_instances[mesh_instance_index];
        let mesh = meshes[mesh_instance.mesh_index];
        let transform = transforms[mesh_instance.transform_index];
        let mvp = mul(projection_view, transform.world);

        if (test_frustum(mvp, mesh.bounds.aabb_center, mesh.bounds.aabb_extent)) {
            meshlet_count = mesh.lods[lod_index].meshlet_count;
        }
    }

    let lane_meshlet_offset = WavePrefixSum(meshlet_count);
    let total_meshlet_count = WaveActiveSum(meshlet_count);

    var base = 0u;
    if (WaveIsFirstLane()) {
        base = __atomic_add(visibility[0], total_meshlet_count, MemoryOrder::Relaxed);
        let new_total = base + total_meshlet_count;
        let needed_groups = (new_total + CULLING_MESHLET_COUNT - 1u) / CULLING_MESHLET_COUNT;
        __atomic_max(cull_meshlets_cmd[0].x, needed_groups, MemoryOrder::Relaxed);
    }
    base = WaveReadLaneFirst(base);

    if (meshlet_count > 0u) {
        mesh_instances[mesh_instance_index].lod_index = lod_index;

        let base_offset = base + lane_meshlet_offset;
        for (u32 i = 0u; i < meshlet_count; i++) {
            let offset = base_offset + i;
            meshlet_instances[offset].mesh_instance_index = mesh_instance_index;
            meshlet_instances[offset].meshlet_index = i;
        }
    }
}