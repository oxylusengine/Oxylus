import common;
import gpu;
import scene;
import cull;
import debug_drawer;

struct ShaderParameters {
    ConstantBuffer<Camera> camera;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;

    RWStructuredBuffer<MeshInstance> mesh_instances;
    RWStructuredBuffer<MeshletInstance> meshlet_instances;
    RWStructuredBuffer<u32> meshlet_instances_count;
};

#ifndef CULLING_MESHES_COUNT
    #define CULLING_MESHES_COUNT 64
#endif

[[shader("compute")]]
[[numthreads(CULLING_MESHES_COUNT, 1, 1)]]
func cs_main(
    uint3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform u32 mesh_instances_count,
    uniform CullFlags cull_flags
) -> void {
    let mesh_instance_index = thread_id.x;
    if (mesh_instance_index >= mesh_instances_count) {
        return;
    }

    let mesh_instance = &params.mesh_instances[mesh_instance_index];
    let mesh = params.meshes[mesh_instance.mesh_index];
    let transform = params.transforms[mesh_instance.transform_index];
    let mvp = mul(params.camera.projection_view, transform.world);
    if (!test_frustum(mvp, mesh.bounds.aabb_center, mesh.bounds.aabb_extent)) {
        return;
    }

    var lod_index = 0;
    if (true) {
        // Credits:
        // - https://github.com/Sunset-Flock/Timberdoodle/blob/786f141e261dff4756e7f1a67dd7f7a5e1277956/src/scene/mesh_lod.hpp#L45
        let aabb_center = mul(transform.world, f32x4(mesh.bounds.aabb_center, 1.0)).xyz;
        let aabb_extent_x = length(transform.world[0]) * mesh.bounds.aabb_extent.x;
        let aabb_extent_y = length(transform.world[1]) * mesh.bounds.aabb_extent.y;
        let aabb_extent_z = length(transform.world[2]) * mesh.bounds.aabb_extent.z;
        let aabb_rough_extent = max(max(aabb_extent_x, aabb_extent_y), aabb_extent_z);
        let aabb_rough_camera_distance = max(length(aabb_center - params.camera.position.xyz) - 0.5 * aabb_rough_extent, 0.0);

        // Avoiding the atan here
        let rough_resolution = max(params.camera.resolution.x, params.camera.resolution.y);
        let fov90_distance_to_screen_ratio = 2.0f;
        let pixel_size_at_1m = fov90_distance_to_screen_ratio / rough_resolution;
        let aabb_size_at_1m = (aabb_rough_extent / aabb_rough_camera_distance);
        let rough_aabb_pixel_size = aabb_size_at_1m / pixel_size_at_1m;

        for (var i = 1; i < mesh.lod_count; i++) {
            let mesh_lod = mesh.lods[i];
            let rough_pixel_error = rough_aabb_pixel_size * mesh_lod.error;
            constexpr f32 acceptable_lod_error = 2.0;
            if (rough_pixel_error < acceptable_lod_error) {
                lod_index = i;
            } else {
                break;
            }
        }
    }

    mesh_instance.lod_index = lod_index;
    let mesh_lod = mesh.lods[lod_index];
    let meshlet_instance_offset = com::atomic_add(params.meshlet_instances_count[0], mesh_lod.meshlet_count, com::memory_order_relaxed);
    for (u32 i = 0; i < mesh_lod.meshlet_count; i++) {
        let offset = meshlet_instance_offset + i;
        var meshlet_instance = MeshletInstance();
        meshlet_instance.mesh_instance_index = mesh_instance_index;
        meshlet_instance.meshlet_index = i;
        params.meshlet_instances[offset] = meshlet_instance;
    }
}
