module cull_meshlets;

import common;
import gpu;
import scene;

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

func test_aabb_vs_plane(f32x3 center, f32x3 extent, f32x4 plane) -> bool {
    const f32x3 normal = plane.xyz;
    const f32 radius = dot(extent, abs(normal));
    return (dot(normal, center) - plane.w) >= -radius;
}

func normalize_plane(f32x4 p) -> f32x4 {
    return p / length(p.xyz);
}

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

vector<T, N> min8<T : __BuiltinFloatingPointType, let N : int>(
    vector<T, N> p0, vector<T, N> p1, vector<T, N> p2,
    vector<T, N> p3, vector<T, N> p4, vector<T, N> p5,
    vector<T, N> p6, vector<T, N> p7) {
    return min(p0, min(p1, min(p2, min(p3, min(p4, min(p5, min(p6, p7)))))));
}

vector<T, N> max8<T : __BuiltinFloatingPointType, let N : int>(
    vector<T, N> p0, vector<T, N> p1, vector<T, N> p2,
    vector<T, N> p3, vector<T, N> p4, vector<T, N> p5,
    vector<T, N> p6, vector<T, N> p7) {
    return max(p0, max(p1, max(p2, max(p3, max(p4, max(p5, max(p6, p7)))))));
}

// https://zeux.io/2023/01/12/approximate-projected-bounds
func project_aabb(f32x4x4 mvp, f32 near, f32x3 aabb_center, f32x3 aabb_extent) -> Optional<MeshletBounds> {
    let SX = mul(mvp, f32x4(aabb_extent.x, 0.0, 0.0, 0.0));
    let SY = mul(mvp, f32x4(0.0, aabb_extent.y, 0.0, 0.0));
    let SZ = mul(mvp, f32x4(0.0, 0.0, aabb_extent.z, 0.0));

    let aabb_half_extent = aabb_extent * 0.5;
    let P0 = mul(mvp, f32x4(aabb_center - aabb_half_extent, 1.0));
    let P1 = P0 + SZ;
    let P2 = P0 + SY;
    let P3 = P2 + SZ;
    let P4 = P0 + SX;
    let P5 = P4 + SZ;
    let P6 = P4 + SY;
    let P7 = P6 + SZ;

    let depth = min8(P0, P1, P2, P3, P4, P5, P6, P7).w;
    if (depth < near)
        return none;

    let DP0 = P0.xyz / P0.w;
    let DP1 = P1.xyz / P1.w;
    let DP2 = P2.xyz / P2.w;
    let DP3 = P3.xyz / P3.w;
    let DP4 = P4.xyz / P4.w;
    let DP5 = P5.xyz / P5.w;
    let DP6 = P6.xyz / P6.w;
    let DP7 = P7.xyz / P7.w;
    let min = min8(DP0, DP1, DP2, DP3, DP4, DP5, DP6, DP7);
    let max = max8(DP0, DP1, DP2, DP3, DP4, DP5, DP6, DP7);
    var vaabb = f32x4(min.xy, max.xy);
    vaabb = vaabb.xwzy * f32x4(0.5f, -0.5f, 0.5f, -0.5f) + 0.5f;
    return MeshletBounds(f32x3(vaabb.xy, min.z), f32x3(vaabb.zw, max.z));
}

func test_occlusion(
    in ShaderParameters params,
    in f32x4x4 mvp,
    in f32 near_clip,
    in f32x2 resolution,
    f32x3 aabb_center,
    f32x3 aabb_extent
) -> bool {
    if (let ndc_aabb = project_aabb(mvp, near_clip, aabb_center, aabb_extent)) {
        MeshletBounds aabb = ndc_aabb;
        let scale = resolution * 0.5f;
        aabb.aabb_min.xy = aabb.aabb_min.xy * scale;
        aabb.aabb_max.xy = aabb.aabb_max.xy * scale;

        let hzb_size = 1 << firstbithigh(u32x2(resolution) - 1);

        let min_texel = u32x2(max(aabb.aabb_min.xy, 0.f));
        let max_texel = u32x2(min(aabb.aabb_max.xy, hzb_size - 1.f));
        let size = max_texel - min_texel + 1;
        let max_size = max(size.x, size.y);
    
        var mip = firstbithigh(max_size - 1) - 1;
        let smin = min_texel >> mip;
        let smax = max_texel >> mip;
        if (any(smax - smin > 1)) {
            mip += 1;
        }
    
        let uv = ((f32x2(min_texel) + f32x2(max_texel)) * 0.5f) / hzb_size;
        let d = params.hiz_image.sample_mip(params.hiz_sampler, uv, mip);
        return aabb.aabb_max.z <= d;
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

    let mvp = mul(params.camera.previous_projection_view, transform.world);
    var meshlet_passed = true;
    // Frustum culling
    if (cull_flags & CullFlags::MeshletFrustum) {
        meshlet_passed = test_frustum(mvp, aabb_center, aabb_extent);
    }

    if (meshlet_passed) {
       meshlet_passed = test_occlusion(params, mvp, params.camera.near_clip, params.camera.resolution, aabb_center, aabb_extent);
    }

    if (meshlet_passed) {
        let index = com::atomic_add(params.cull_triangles_cmd[0].x, 1, com::memory_order_relaxed);
        params.visible_meshlet_instances_indices[index] = meshlet_instance_index;
    }
}