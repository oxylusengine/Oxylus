import common;
import gpu;
import scene;
import cull;
import debug_drawer;

struct ShaderParameters {
    ConstantBuffer<Camera> camera;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<MeshInstance> mesh_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;
    Image2D<f32> hiz_image;
    Sampler hiz_sampler;
    StructuredBuffer<u32> meshlet_instances_count;

    RWStructuredBuffer<DispatchIndirectCommand> cull_triangles_cmd;
    RWStructuredBuffer<u32> visible_meshlet_instances_indices;
};

#ifndef CULLING_MESHLET_COUNT
    #define CULLING_MESHLET_COUNT 64
#endif

[[shader("compute")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func cs_main(
    uint3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform CullFlags cull_flags
) -> void {
    let meshlet_instance_count = params.meshlet_instances_count[0];
    let meshlet_instance_index = thread_id.x;
    if (meshlet_instance_index >= meshlet_instance_count) {
        return;
    }

    let meshlet_instance = params.meshlet_instances[meshlet_instance_index];
    let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];
    let mesh = params.meshes[mesh_instance.mesh_index];
    let transform = params.transforms[mesh_instance.transform_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let bounds = mesh_lod.meshlet_bounds[meshlet_instance.meshlet_index];

    var visible = true;
    if (visible && (cull_flags & CullFlags::MeshletFrustum)) {
        let cur_mvp = mul(params.camera.projection_view, transform.world);
        visible = test_frustum(cur_mvp, bounds.aabb_center, bounds.aabb_extent);
    }

    if (visible && (cull_flags & CullFlags::OcclusionCulling)) {
        let prev_mvp = mul(params.camera.previous_projection_view, transform.world);
        if (let screen_aabb = project_aabb(prev_mvp, params.camera.near_clip, bounds.aabb_center, bounds.aabb_extent)) {
            visible = !test_occlusion(screen_aabb, params.hiz_image, params.hiz_sampler);
        }
    }

    if (visible) {
        let index = com::atomic_add(params.cull_triangles_cmd[0].x, 1, com::memory_order_relaxed);
        params.visible_meshlet_instances_indices[index] = meshlet_instance_index;
    }
}
