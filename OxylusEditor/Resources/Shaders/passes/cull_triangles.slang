module cull_triangles;

import common;
import gpu;
import scene;
import visbuffer;

struct ShaderParameters {
    RWStructuredBuffer<DrawIndexedIndirectCommand> draw_cmd;
    ConstantBuffer<Camera> camera;
    StructuredBuffer<u32> visible_meshlet_instances_indices;
    RWStructuredBuffer<u32> reordered_indices;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;
};

groupshared u32 base_index_shared;
groupshared u32 triangles_passed_shared;
groupshared mat4 model_view_proj_shared;

// https://github.com/GPUOpen-Effects/GeometryFX/blob/master/amd_geometryfx/src/Shaders/AMD_GeometryFX_Filtering.hlsl
// Parameters: vertices in UV space, viewport extent
func CullSmallPrimitive(f32x2x3 vertices, f32x2 viewportExtent) -> bool {
    const uint SUBPIXEL_BITS = 8;
    const uint SUBPIXEL_MASK = 0xFF;
    const uint SUBPIXEL_SAMPLES = 1 << SUBPIXEL_BITS;
    /**
    Computing this in float-point is not precise enough
    We switch to a 23.8 representation here which should match the
    HW subpixel resolution.
    We use a 8-bit wide guard-band to avoid clipping. If
    a triangle is outside the guard-band, it will be ignored.

    That is, the actual viewport supported here is 31 bit, one bit is
    unused, and the guard band is 1 << 23 bit large (8388608 pixels)
    */

    i32x2 minBB = i32x2(1 << 30, 1 << 30);
    i32x2 maxBB = i32x2(-(1 << 30), -(1 << 30));

    for (uint i = 0; i < 3; ++i) {
        f32 v = reinterpret<f32>(1 << 23);
        f32x2 screenSpacePositionFP = vertices[i] * viewportExtent;
        // Check if we would overflow after conversion
        if (screenSpacePositionFP.x < -v || screenSpacePositionFP.x > v || screenSpacePositionFP.y < -v || screenSpacePositionFP.y > v) {
            return true;
        }

        let screenSpacePosition = i32x2(screenSpacePositionFP * SUBPIXEL_SAMPLES);
        minBB = min(screenSpacePosition, minBB);
        maxBB = max(screenSpacePosition, maxBB);
    }

    /**
    Test is:

    Is the minimum of the bounding box right or above the sample
    point and is the width less than the pixel width in samples in
    one direction.

    This will also cull very long triangles which fall between
    multiple samples.
    */
    return !(
        (((minBB.x & SUBPIXEL_MASK) > SUBPIXEL_SAMPLES / 2)
         && ((maxBB.x - ((minBB.x & ~SUBPIXEL_MASK) + SUBPIXEL_SAMPLES / 2)) < (SUBPIXEL_SAMPLES - 1)))
        || (((minBB.y & SUBPIXEL_MASK) > SUBPIXEL_SAMPLES / 2)
            && ((maxBB.y - ((minBB.y & ~SUBPIXEL_MASK) + SUBPIXEL_SAMPLES / 2)) < (SUBPIXEL_SAMPLES - 1)))
    );
}

func test_triangle(in Camera camera, in Mesh mesh, in Meshlet meshlet, u32 triangle_index, CullFlags cull_flags) -> bool {
    const Triangle tri = meshlet.indices(mesh, triangle_index);
    const u32x3 vertices = meshlet.vertices(mesh, tri);
    const f32x3x3 positions = meshlet.positions(mesh, vertices);

    const f32x4 clip_pos_0 = mul(model_view_proj_shared, f32x4(positions[0], 1.0));
    const f32x4 clip_pos_1 = mul(model_view_proj_shared, f32x4(positions[1], 1.0));
    const f32x4 clip_pos_2 = mul(model_view_proj_shared, f32x4(positions[2], 1.0));

    // Cull all triangles behind camera
    if (clip_pos_0.z < 0.0 && clip_pos_1.z < 0.0 && clip_pos_2.z < 0.0) {
        return false;
    }

    if (cull_flags & CullFlags::TriangleBackFace) {
        // https://zeux.io/2023/04/28/triangle-backface-culling/#fnref:3
        const bool is_backfacing = determinant(f32x3x3(clip_pos_0.xyw, clip_pos_1.xyw, clip_pos_2.xyw)) >= 0.0001;
        if (is_backfacing) {
            return false;
        }
    }

    if (cull_flags & CullFlags::MicroTriangles) {
        const float3x2 uv_pos = { clip_pos_0.xy * 0.5 + 0.5, clip_pos_1.xy * 0.5 + 0.5, clip_pos_2.xy * 0.5 + 0.5 };
        if (!CullSmallPrimitive(uv_pos, camera.resolution)) {
            return false;
        }
    }

    return true;
}

// Shut up LSP
#ifndef CULLING_TRIANGLE_COUNT
    #define CULLING_TRIANGLE_COUNT 64
#endif

[[shader("compute")]]
[[numthreads(CULLING_TRIANGLE_COUNT, 1, 1)]]
func cs_main(
    uint3 group_id : SV_GroupID,
    uint3 group_thread_id : SV_GroupThreadID,
    uniform CullFlags cull_flags,
    uniform ParameterBlock<ShaderParameters> params,
) -> void {
    const u32 local_index = group_thread_id.x;
    const u32 triangle_index = local_index * 3;
    const u32 visible_meshlet_index = group_id.x;
    const u32 meshlet_instance_index = params.visible_meshlet_instances_indices[visible_meshlet_index];
    const MeshletInstance meshlet_instance = params.meshlet_instances[meshlet_instance_index];
    const Mesh mesh = params.meshes[meshlet_instance.mesh_index];
    const u32 meshlet_index = meshlet_instance.meshlet_index;
    const Meshlet meshlet = mesh.meshlets[meshlet_index];

    if (local_index == 0) {
        triangles_passed_shared = 0;
        const Transform transform = params.transforms[meshlet_instance.transform_index];
        model_view_proj_shared = mul(params.camera.projection_view, transform.world);
    }

    com::control_barrier(com::memory_order_acq_rel);

    bool triangle_passed = false;
    u32 active_triangle_index = 0;
    if (local_index < meshlet.triangle_count) {
        if (cull_flags & CullFlags::TriangleCulling) {
            triangle_passed = test_triangle(params.camera, mesh, meshlet, local_index, cull_flags);
        }
        if (triangle_passed) {
            active_triangle_index = com::atomic_add(triangles_passed_shared, 1, com::memory_order_relaxed);
        }
    }

    com::control_barrier(com::memory_order_acq_rel);

    if (local_index == 0) {
        base_index_shared = com::atomic_add(params.draw_cmd[0].index_count, triangles_passed_shared * 3, com::memory_order_relaxed);
    }

    com::control_barrier(com::memory_order_acq_rel);

    if (triangle_passed) {
        const u32 index_offset = base_index_shared + active_triangle_index * 3;
        params.reordered_indices[index_offset + 0] = (visible_meshlet_index << MESHLET_PRIMITIVE_BITS) | ((triangle_index + 0) & MESHLET_PRIMITIVE_MASK);
        params.reordered_indices[index_offset + 1] = (visible_meshlet_index << MESHLET_PRIMITIVE_BITS) | ((triangle_index + 1) & MESHLET_PRIMITIVE_MASK);
        params.reordered_indices[index_offset + 2] = (visible_meshlet_index << MESHLET_PRIMITIVE_BITS) | ((triangle_index + 2) & MESHLET_PRIMITIVE_MASK);
    }
}
