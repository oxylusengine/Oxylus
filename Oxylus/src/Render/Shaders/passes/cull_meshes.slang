import common;
import gpu;
import scene;
import cull;
import debug_drawer;

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<Transform> transforms;
[[vk::binding(2)]] RWStructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] RWStructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(4)]] RWStructuredBuffer<u32> visible_meshlet_instances_count;
[[vk::binding(5)]] RWStructuredBuffer<DebugDrawer> debug_drawer;

#ifndef CULLING_MESH_COUNT
#define CULLING_MESH_COUNT 64
#endif

[[shader("compute")]]
[[numthreads(CULLING_MESH_COUNT, 1, 1)]]
func cs_main(
    uint3 thread_id : SV_DispatchThreadID,
    uniform u32 mesh_instances_count,
    uniform CullFlags cull_flags,
    uniform f32x4x4 frustum_projection_view,
    uniform f32x3 observer_position,
    uniform f32 observer_max_resolution,
    uniform f32 observer_acceptable_lod_error
) -> void {
    let mesh_instance_index = thread_id.x;
    if (mesh_instance_index >= mesh_instances_count) {
        return;
    }

    let mesh_instance = &mesh_instances[mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let transform = transforms[mesh_instance.transform_index];
    let mvp = mul(frustum_projection_view, transform.world);

    let cull_frustum = (cull_flags & CullFlags::MeshFrustum) != 0;
    if (cull_frustum && !test_frustum(mvp, mesh.bounds.aabb_center, mesh.bounds.aabb_extent)) {
        return;
    }

    var lod_index = 0;
#if 1
    // Credits:
    // - https://github.com/Sunset-Flock/Timberdoodle/blob/786f141e261dff4756e7f1a67dd7f7a5e1277956/src/scene/mesh_lod.hpp#L45
    let aabb_center = mul(transform.world, f32x4(mesh.bounds.aabb_center, 1.0)).xyz;
    let aabb_extent_x = length(transform.world[0]) * mesh.bounds.aabb_extent.x;
    let aabb_extent_y = length(transform.world[1]) * mesh.bounds.aabb_extent.y;
    let aabb_extent_z = length(transform.world[2]) * mesh.bounds.aabb_extent.z;
    let aabb_rough_extent = max(max(aabb_extent_x, aabb_extent_y), aabb_extent_z);
    let aabb_rough_camera_distance = max(length(aabb_center - observer_position) - 0.5 * aabb_rough_extent, 0.0);

    let fov90_distance_to_screen_ratio = 2.0f;
    let pixel_size_at_1m = fov90_distance_to_screen_ratio / observer_max_resolution;
    let aabb_size_at_1m = (aabb_rough_extent / aabb_rough_camera_distance);
    let rough_aabb_pixel_size = aabb_size_at_1m / pixel_size_at_1m;

    for (var i = 1; i < mesh.lod_count; i++) {
        let mesh_lod = mesh.lods[i];
        let rough_pixel_error = rough_aabb_pixel_size * mesh_lod.error;
        if (rough_pixel_error < observer_acceptable_lod_error) {
            lod_index = i;
        } else {
            break;
        }
    }
#endif
    mesh_instance.lod_index = lod_index;
    let mesh_lod = mesh.lods[lod_index];
    let meshlet_count = mesh_lod.meshlet_count;
    var base_meshlet_instance_offset = __atomic_add(visible_meshlet_instances_count[0], meshlet_count, MemoryOrder::Relaxed);
    for (u32 i = 0; i < meshlet_count; i++) {
        let offset = base_meshlet_instance_offset + i;
        meshlet_instances[offset].mesh_instance_index = mesh_instance_index;
        meshlet_instances[offset].meshlet_index = i;
    }
}