import common;
import gpu;
import scene;
import cull;

#include <defines.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(3)]] StructuredBuffer<Transform> transforms;
[[vk::binding(4)]] StructuredBuffer<u32> visibility;
[[vk::binding(5)]] RWStructuredBuffer<u32> visible_meshlet_instances_indices;
[[vk::binding(6)]] RWStructuredBuffer<DispatchIndirectCommand> cull_triangles_cmd;

[[shader("compute")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func cs_main(
    uint global_thread_id : SV_DispatchThreadID,
    uniform f32x4x4 projection_view,
) -> void {
    let meshlet_instance_count = visibility[0];
    let meshlet_instance_index = global_thread_id;
    if (meshlet_instance_index >= meshlet_instance_count) {
        return;
    }

    let meshlet_instance = meshlet_instances[meshlet_instance_index];
    let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
    let transform = transforms[mesh_instance.transform_index];
    let mvp = mul(projection_view, transform.world);

    let mesh = meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let bounds = mesh_lod.meshlet_bounds[meshlet_instance.meshlet_index];

    if (test_frustum(mvp, bounds.aabb_center, bounds.aabb_extent)) {
        let index = __atomic_add(cull_triangles_cmd[0].x, 1, MemoryOrder::Relaxed);
        visible_meshlet_instances_indices[index] = meshlet_instance_index;
    }
}
