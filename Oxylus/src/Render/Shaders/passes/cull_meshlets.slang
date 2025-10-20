import common;
import gpu;
import scene;
import cull;

#include <defines.slang>

[[vk::constant_id(0)]] const bool LATE = false;
[[vk::constant_id(1)]] const bool OCCLUSION_CULL = true;
[[vk::binding(0)]] Image2D<f32> hiz_image;
[[vk::binding(1)]] Sampler hiz_sampler;
[[vk::binding(2)]] ConstantBuffer<Camera> camera;
[[vk::binding(3)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(4)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(5)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(6)]] StructuredBuffer<Transform> transforms;
[[vk::binding(7)]] RWStructuredBuffer<MeshletInstanceVisibility> visibility;
[[vk::binding(8)]] RWStructuredBuffer<u32> visible_meshlet_instances_indices;
[[vk::binding(9)]] RWStructuredBuffer<u32> meshlet_instance_visibility_mask;
[[vk::binding(10)]] RWStructuredBuffer<DispatchIndirectCommand> cull_triangles_cmd;

[[shader("compute")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func cs_main(
    uint global_thread_id : SV_DispatchThreadID,
) -> void {
    let meshlet_instance_index = global_thread_id;
    if (meshlet_instance_index >= visibility[0].total_visible_meshlet_instances) {
        return;
    }

    let meshlet_instance = meshlet_instances[meshlet_instance_index];
    let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
    let transform = transforms[mesh_instance.transform_index];
    let mvp = mul(camera.projection_view, transform.world);

    let mesh = meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let bounds = mesh_lod.meshlet_bounds[meshlet_instance.meshlet_index];

    var visibility_mask_index = 0;
    var visibility_bit = 0;
    var was_visible = true;
    if (OCCLUSION_CULL) {
        let meshlet_instance_visibility_index = mesh_instance.meshlet_instance_visibility_offset + meshlet_instance.meshlet_index;
        visibility_mask_index = meshlet_instance_visibility_index / 32;
        let bit_index = meshlet_instance_visibility_index - visibility_mask_index * 32;
        visibility_bit = 1 << bit_index;
        was_visible = (meshlet_instance_visibility_mask[visibility_mask_index] & visibility_bit) != 0;
    }

    var visible = LATE ? true : was_visible;
    visible = visible && test_frustum(mvp, bounds.aabb_center, bounds.aabb_extent);

    if (OCCLUSION_CULL && LATE && visible) {
        if (let screen_aabb = project_aabb(mvp, camera.near_clip, bounds.aabb_center, bounds.aabb_extent)) {
            visible = !test_occlusion(screen_aabb, hiz_image, hiz_sampler);
        }
    }

    if (visible && (!LATE || !was_visible)) {
        var index = 0;
        if (!LATE) {
            index = __atomic_add(visibility[0].early_visible_meshlet_instances, 1, MemoryOrder::Relaxed);
        } else {
            let late_offset = __atomic_add(visibility[0].late_visible_meshlet_instances, 1, MemoryOrder::Relaxed);
            index = late_offset + visibility[0].early_visible_meshlet_instances;
        }

        visible_meshlet_instances_indices[index] = meshlet_instance_index;

        __atomic_add(cull_triangles_cmd[0].x, 1, MemoryOrder::Relaxed);
    }

    if (OCCLUSION_CULL && LATE) {
        if (visible) {
            __atomic_or(meshlet_instance_visibility_mask[visibility_mask_index], visibility_bit, MemoryOrder::Relaxed);
        } else {
            __atomic_and(meshlet_instance_visibility_mask[visibility_mask_index], ~visibility_bit, MemoryOrder::Relaxed);
        }
    }
}
