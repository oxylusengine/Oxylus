import common;
import gpu;
import scene;
import cull;

#include <defines.slang>

[[vk::constant_id(0)]] const u32 CULL_FLAGS;
[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(3)]] StructuredBuffer<Transform> transforms;
[[vk::binding(4)]] RWStructuredBuffer<MeshletInstanceVisibility> visibility;
[[vk::binding(5)]] RWStructuredBuffer<u32> visible_meshlet_instances_indices;
[[vk::binding(6)]] RWStructuredBuffer<DispatchIndirectCommand> cull_triangles_cmd;

groupshared u32 visible_count_shared;
groupshared u32 base_index_shared;
groupshared u32 meshlet_indices_shared[CULLING_MESHLET_COUNT];

[[shader("compute")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func cs_main(
    uint global_thread_id : SV_DispatchThreadID,
    uint group_thread_id : SV_GroupThreadID,
    uniform CullCamera camera
) -> void {
    if (group_thread_id == 0) {
        visible_count_shared = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    var local_slot = ~0u;
    let meshlet_instance_index = global_thread_id;
    if (meshlet_instance_index < visibility[0].total_visible_meshlet_instances) {
        let meshlet_instance = meshlet_instances[meshlet_instance_index];
        let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
        let transform = transforms[mesh_instance.transform_index];
        let mvp = mul(camera.projection_view, transform.world);

        let mesh = meshes[mesh_instance.mesh_index];
        let mesh_lod = mesh.lods[mesh_instance.lod_index];
        let meshlet_bounds = mesh_lod.meshlet_bounds[meshlet_instance.meshlet_index];
        let aabb_center = meshlet_bounds.get_aabb_center();
        let aabb_extent = meshlet_bounds.get_aabb_extent();
        let cone_cutoff = meshlet_bounds.get_cone_cutoff();

        let cone_axis = normalize(transform.to_world_normal(meshlet_bounds.get_cone_axis()));
        let world_center = transform.to_world_position(aabb_center).xyz;
        let world_radius = transform.to_world_radius(length(aabb_extent * 0.5));
        let cone_visible = cone_cutoff >= 1.0 || !test_cone(world_center, world_radius, cone_axis, cone_cutoff, camera.position);

        if (cone_visible && test_frustum(mvp, aabb_center, aabb_extent)) {
            local_slot = __atomic_add(visible_count_shared, 1, MemoryOrder::Relaxed);
            meshlet_indices_shared[local_slot] = meshlet_instance_index;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (visible_count_shared > 0) {
        if (group_thread_id == 0) {
            base_index_shared = __atomic_add(cull_triangles_cmd[0].x, visible_count_shared, MemoryOrder::Relaxed);
        }

        GroupMemoryBarrierWithGroupSync();

        if (local_slot < visible_count_shared) {
            visible_meshlet_instances_indices[base_index_shared + local_slot] = meshlet_indices_shared[local_slot];
        }
    }
}
