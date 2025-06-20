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
};

func test_aabb_vs_plane(f32x3 center, f32x3 extent, f32x4 plane) -> bool {
    const f32x3 normal = plane.xyz;
    const f32 radius = dot(extent, abs(normal));
    return (dot(normal, center) - plane.w) >= -radius;
}

func test_frustum(in Camera camera, in mat4 world_transform, in f32x3 aabb_center, in f32x3 aabb_extent) -> bool {
    const f32x3 world_aabb_center = mul(world_transform, f32x4(aabb_center, 1.0)).xyz;
    const f32x3 right = world_transform[0].xyz * aabb_extent.x;
    const f32x3 up = world_transform[1].xyz * aabb_extent.y;
    const f32x3 forward = -world_transform[2].xyz * aabb_extent.z;
    const f32x3 world_extent = {
        abs(dot(f32x3(1.0, 0.0, 0.0), right)) + abs(dot(f32x3(1.0, 0.0, 0.0), up)) + abs(dot(f32x3(1.0, 0.0, 0.0), forward)),
        abs(dot(f32x3(0.0, 1.0, 0.0), right)) + abs(dot(f32x3(0.0, 1.0, 0.0), up)) + abs(dot(f32x3(0.0, 1.0, 0.0), forward)),
        abs(dot(f32x3(0.0, 0.0, 1.0), right)) + abs(dot(f32x3(0.0, 0.0, 1.0), up)) + abs(dot(f32x3(0.0, 0.0, 1.0), forward))
    };

    [[unroll]]
    for (uint i = 0; i < 6; i++) {
        if (!test_aabb_vs_plane(world_aabb_center, world_extent, camera.frustum_planes[i])) {
            return false;
        }
    }

    return true;
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
    const u32 meshlet_instance_index = thread_id.x;
    if (meshlet_instance_index >= meshlet_instance_count) {
        return;
    }

    const MeshletInstance meshlet_instance = params.meshlet_instances[meshlet_instance_index];
    const Mesh mesh = params.meshes[meshlet_instance.mesh_index];
    const u32 meshlet_index = meshlet_instance.meshlet_index;
    const Meshlet meshlet = mesh.meshlets[meshlet_index];
    const Transform transform = params.transforms[meshlet_instance.transform_index];
    const MeshletBounds bounds = mesh.meshlet_bounds[meshlet_index];
    const f32x3 aabb_min = bounds.aabb_min;
    const f32x3 aabb_max = bounds.aabb_max;
    const f32x3 aabb_center = (aabb_min + aabb_max) * 0.5;
    const f32x3 aabb_extent = aabb_max - aabb_center;

    bool meshlet_passed = true;
    // Frustum culling
    if (cull_flags & CullFlags::MeshletFrustum) {
        meshlet_passed = test_frustum(params.camera, transform.world, aabb_center, aabb_extent);
    }

    if (meshlet_passed) {
        u32 index = com::atomic_add(params.cull_triangles_cmd[0].x, 1, com::memory_order_relaxed);
        params.visible_meshlet_instances_indices[index] = meshlet_instance_index;
    }
}