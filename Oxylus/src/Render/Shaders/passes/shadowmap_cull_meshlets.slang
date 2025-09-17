import common;
import gpu;
import scene;
import cull;

[[vk::binding(0)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(3)]] StructuredBuffer<Transform> transforms;
[[vk::binding(4)]] StructuredBuffer<u32> all_visible_meshlet_instances_count;
[[vk::binding(5)]] StructuredBuffer<u32> meshlet_instance_visibility_mask;
[[vk::binding(6)]] RWStructuredBuffer<u32> visible_meshlet_instances_count;
[[vk::binding(7)]] RWStructuredBuffer<u32> visible_meshlet_instances_indices;
[[vk::binding(8)]] RWStructuredBuffer<DispatchIndirectCommand> cull_triangles_cmd;

#ifndef CULLING_MESHLET_COUNT
#define CULLING_MESHLET_COUNT 64
#endif

[[shader("compute")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func cs_main(
    uint group_thread_id : SV_GroupThreadID,
    uint global_thread_id : SV_DispatchThreadID,
    uniform f32x4x4 projection_view,
) -> void {
    let meshlet_instance_count = all_visible_meshlet_instances_count[0];
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

    let meshlet_instance_visibility_index = mesh_instance.meshlet_instance_visibility_offset + meshlet_instance.meshlet_index;
    let visibility_mask_index = meshlet_instance_visibility_index / 32;
    let bit_index = meshlet_instance_visibility_index - visibility_mask_index * 32;
    let visibility_bit = 1 << bit_index;
    let is_visible = (meshlet_instance_visibility_mask[visibility_mask_index] & visibility_bit) != 0;

    if (is_visible && test_frustum(mvp, bounds.aabb_center, bounds.aabb_extent)) {
        let index = __atomic_add(visible_meshlet_instances_count[0], 1, MemoryOrder::Relaxed);
        visible_meshlet_instances_indices[index] = meshlet_instance_index;

        __atomic_add(cull_triangles_cmd[0].x, 1, MemoryOrder::Relaxed);
    }
}
