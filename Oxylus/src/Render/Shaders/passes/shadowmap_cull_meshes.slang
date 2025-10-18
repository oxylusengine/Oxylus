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
    if (mesh_instance_index >= mesh_instances_count) {
        return;
    }

    let mesh_instance = mesh_instances[mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let transform = transforms[mesh_instance.transform_index];
    let mvp = mul(projection_view, transform.world);

    if (!test_frustum(mvp, mesh.bounds.aabb_center, mesh.bounds.aabb_extent)) {
        return;
    }

    let lod_index = 0; // TODO: VSM LODs
    mesh_instances[mesh_instance_index].lod_index = lod_index;
    let mesh_lod = mesh.lods[lod_index];
    let meshlet_count = mesh_lod.meshlet_count;
    var base_meshlet_instance_offset = __atomic_add(visibility[0], meshlet_count, MemoryOrder::Relaxed);
    for (u32 i = 0; i < meshlet_count; i++) {
        let offset = base_meshlet_instance_offset + i;
        meshlet_instances[offset].mesh_instance_index = mesh_instance_index;
        meshlet_instances[offset].meshlet_index = i;
    }
}

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func generate_commands_cs_main() -> void {
    cull_meshlets_cmd[0].x = (visibility[0] + (CULLING_MESHLET_COUNT - 1)) / CULLING_MESHLET_COUNT;
}
