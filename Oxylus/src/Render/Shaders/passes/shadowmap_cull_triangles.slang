import common;
import gpu;
import scene;
import cull;
import visbuffer;

#include <defines.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(3)]] StructuredBuffer<Transform> transforms;
[[vk::binding(4)]] StructuredBuffer<u32> visible_meshlet_instances_indices;
[[vk::binding(5)]] RWStructuredBuffer<u32> reordered_indices;
[[vk::binding(6)]] RWStructuredBuffer<DrawIndexedIndirectCommand> draw_cmd;

groupshared u32 base_index_shared;
groupshared u32 triangles_passed_shared;
groupshared u32 meshlet_instance_index_shared;
groupshared Mesh mesh_shared;
groupshared MeshLOD mesh_lod_shared;
groupshared Meshlet meshlet_shared;
groupshared mat4 model_view_proj_shared;

[[shader("compute")]]
[[numthreads(CULLING_TRIANGLE_COUNT, 1, 1)]]
func cs_main(
    uint3 group_id : SV_GroupID,
    uint3 group_thread_id : SV_GroupThreadID,
    uniform f32x4x4 projection_view
) -> void {
    var visible_meshlet_instance_index = group_id.x;
    let local_index = group_thread_id.x;
    let triangle_index = local_index * 3;

    if (local_index == 0) {
        triangles_passed_shared = 0;
        meshlet_instance_index_shared = visible_meshlet_instances_indices[visible_meshlet_instance_index];
        let meshlet_instance = meshlet_instances[meshlet_instance_index_shared];
        let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
        mesh_shared = meshes[mesh_instance.mesh_index];
        mesh_lod_shared = mesh_shared.lods[mesh_instance.lod_index];
        meshlet_shared = mesh_lod_shared.meshlets[meshlet_instance.meshlet_index];

        let transform = transforms[mesh_instance.transform_index];
        model_view_proj_shared = mul(projection_view, transform.world);
    }

    GroupMemoryBarrierWithGroupSync();

    var triangle_passed = false;
    var active_triangle_index = 0;
    if (local_index < meshlet_shared.triangle_count) {
        let indices = meshlet_shared.indices(mesh_lod_shared, local_index);
        let positions = meshlet_shared.positions(mesh_shared, indices);
        let clip_pos = f32x4x3(
            mul(model_view_proj_shared, f32x4(positions[0], 1.0)),
            mul(model_view_proj_shared, f32x4(positions[1], 1.0)),
            mul(model_view_proj_shared, f32x4(positions[2], 1.0))
        );

        triangle_passed = clip_pos[0].z >= 0.0 && clip_pos[1].z >= 0.0 && clip_pos[2].z >= 0.0;
        triangle_passed = triangle_passed && !test_triangle_backface(clip_pos);
        if (triangle_passed) {
            active_triangle_index = __atomic_add(triangles_passed_shared, 1, MemoryOrder::Relaxed);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (local_index == 0) {
        base_index_shared = __atomic_add(draw_cmd[0].index_count, triangles_passed_shared * 3, MemoryOrder::Relaxed);
    }

    GroupMemoryBarrierWithGroupSync();

    if (triangle_passed) {
        let index_offset = base_index_shared + active_triangle_index * 3;
        let masked_instance_index = meshlet_instance_index_shared << MESHLET_PRIMITIVE_BITS;
        reordered_indices[index_offset + 0] = masked_instance_index | ((triangle_index + 0) & MESHLET_PRIMITIVE_MASK);
        reordered_indices[index_offset + 1] = masked_instance_index | ((triangle_index + 1) & MESHLET_PRIMITIVE_MASK);
        reordered_indices[index_offset + 2] = masked_instance_index | ((triangle_index + 2) & MESHLET_PRIMITIVE_MASK);
    }
}
