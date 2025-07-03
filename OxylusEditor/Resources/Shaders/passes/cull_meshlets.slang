module cull_meshlets;

import common;
import gpu;
import scene;
import cull;

struct ShaderParameters {
    RWStructuredBuffer<DispatchIndirectCommand> cull_triangles_cmd;
    ConstantBuffer<Camera> camera;
    RWStructuredBuffer<u32> visible_meshlet_instances_indices;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;
    Image2D<f32> hiz_image;
    Sampler hiz_sampler;
};

func test_frustum(in f32x4x4 mvp, in f32x3 aabb_center, in f32x3 aabb_extent) -> bool {
    f32x4 planes[] = {
        normalize_plane(mvp[3] + mvp[0]),
        normalize_plane(mvp[3] - mvp[0]),
        normalize_plane(mvp[3] + mvp[1]),
        normalize_plane(mvp[3] - mvp[1]),
        normalize_plane(mvp[2])
    };

    let aabb_half_extent = aabb_extent * 0.5;
    [[unroll]]
    for (uint i = 0; i < planes.getCount(); i++) {
        let flip = asuint(planes[i].xyz) & 0x80000000;
        let sign_flipped = asfloat(asuint(aabb_half_extent) ^ flip);
        if (dot(aabb_center + sign_flipped, planes[i].xyz) <= -planes[i].w) {
            return false;
        }
    }

    return true;
}

func test_occlusion(
    in ShaderParameters params,
    in f32x4x4 mvp,
    f32x3 aabb_min,
    f32x3 aabb_extent
) -> bool {
    if (let ret = project_aabb(mvp, params.camera.near_clip, aabb_min, aabb_extent)) {
        ScreenAabb screen_aabb = ret;
        let hiz_size = ceil(params.camera.resolution * 0.5);
        let aabb_min = screen_aabb.min.xy * hiz_size;
        let aabb_max = screen_aabb.max.xy * hiz_size;
    
        let min_texel = u32x2(max(aabb_min, f32x2(0.0)));
        let max_texel = u32x2(min(aabb_max, hiz_size - 1.0));
        let size = max_texel - min_texel;
        let max_size = max(size.x, size.y);
        var mip = max(firstbithigh(max_size) + 1, 2) - 2;

        if (any((max_texel >> u32x2(mip)) > (min_texel >> u32x2(mip)) + 3)) {
            mip += 1;
        }

        let smin = min_texel >> u32x2(mip);
        let smax = max_texel >> u32x2(mip);
    
        let uv = ((f32x2(smin) + f32x2(smax)) * 0.5f) / hiz_size;
        let d = params.hiz_image.sample_mip(params.hiz_sampler, uv, mip);
        //return screen_aabb.max.z <= d;
        return true;
    }

    return false;
}

#ifndef CULLING_MESHLET_COUNT
    #define CULLING_MESHLET_COUNT 64
#endif

[[shader("compute")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func cs_main(
    uint3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform u32 meshlet_instance_count,
    uniform CullFlags cull_flags
) -> void {
    let meshlet_instance_index = thread_id.x;
    if (meshlet_instance_index >= meshlet_instance_count) {
        return;
    }

    let meshlet_instance = params.meshlet_instances[meshlet_instance_index];
    let mesh = params.meshes[meshlet_instance.mesh_index];
    let meshlet_index = meshlet_instance.meshlet_index;
    let meshlet = mesh.meshlets[meshlet_index];
    let transform = params.transforms[meshlet_instance.transform_index];
    let bounds = mesh.meshlet_bounds[meshlet_index];

    let aabb_min = bounds.aabb_min;
    let aabb_max = bounds.aabb_max;
    let aabb_extent = aabb_max - aabb_min;
    let aabb_center = (aabb_min + aabb_max) * 0.5;

    let cur_mvp = mul(params.camera.projection_view, transform.world);
    var meshlet_passed = true;
    // Frustum culling
    if (cull_flags & CullFlags::MeshletFrustum) {
        meshlet_passed = test_frustum(cur_mvp, aabb_center, aabb_extent);
    }

    if (meshlet_passed) {
        let prev_mvp = mul(params.camera.previous_projection_view, transform.world);
        meshlet_passed = !test_occlusion(params, prev_mvp, aabb_min, aabb_extent);
    }

    if (meshlet_passed) {
        let index = com::atomic_add(params.cull_triangles_cmd[0].x, 1, com::memory_order_relaxed);
        params.visible_meshlet_instances_indices[index] = meshlet_instance_index;
    }
}