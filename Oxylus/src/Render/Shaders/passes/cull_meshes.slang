import common;
import gpu;
import scene;
import cull;
import debug_drawer;

#include <defines.slang>

[[vk::constant_id(0)]] const u32 CULL_FLAGS;
[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(2)]] RWStructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] RWStructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(4)]] RWStructuredBuffer<MeshletInstanceVisibility> visibility;
[[vk::binding(5)]] RWStructuredBuffer<DispatchIndirectCommand> cull_meshlets_cmd;
[[vk::binding(6)]] StructuredBuffer<Material> materials;
[[vk::binding(7)]] RWStructuredBuffer<MeshletInstance> transparent_meshlet_instances;
[[vk::binding(8)]] RWStructuredBuffer<DispatchIndirectCommand> blend_cull_meshlets_cmd;
[[vk::binding(9)]] RWStructuredBuffer<DispatchIndirectCommand> refractor_cull_meshlets_cmd;

[[shader("compute")]]
[[numthreads(CULLING_MESH_COUNT, 1, 1)]]
func cs_main(
    uint3 thread_id : SV_DispatchThreadID,
    uniform CullCamera camera,
) -> void {
    let mesh_instance_index = thread_id.x;

    var meshlet_count = 0u;
    var lod_index = 0u;
    var render_class = MaterialClass::Opaque;

    if (mesh_instance_index < camera.mesh_instance_count) {
        let mesh_instance = mesh_instances[mesh_instance_index];
        var mesh = meshes[mesh_instance.mesh_index];
        // swaps in the animated bounds, so frustum culling and LOD selection see where the mesh
        // actually is this frame rather than where it was authored
        mesh.apply_skin(mesh_instance);
        let transform = transforms[mesh_instance.transform_index];
        let mvp = mul(camera.projection_view, transform.world);

        render_class = materials[mesh_instance.material_index].render_class();

        let is_shadow_pass = HAS_FLAG(CULL_FLAGS, CullFlag.ShadowCastersOnly);
        let is_caster = !is_shadow_pass || (mesh_instance.casts_shadows() && (render_class == MaterialClass::Opaque || render_class == MaterialClass::Blend));
        if (is_caster && HAS_FLAG(CULL_FLAGS, CullFlag.TestFrustum) && test_frustum(mvp, mesh.bounds.aabb_center, mesh.bounds.aabb_extent)) {
            if (HAS_FLAG(CULL_FLAGS, CullFlag.SelectLOD)) {
                let aabb_center = mul(transform.world, f32x4(mesh.bounds.aabb_center, 1.0)).xyz;
                let aabb_extent = abs(mul(transform.world, f32x4(mesh.bounds.aabb_extent, 0.0)).xyz);
                // Credits:
                // - https://github.com/Sunset-Flock/Timberdoodle/blob/786f141e261dff4756e7f1a67dd7f7a5e1277956/src/scene/mesh_lod.hpp#L45
                let aabb_rough_extent = max(aabb_extent.x, max(aabb_extent.y, aabb_extent.z));
                let aabb_rough_camera_distance = max(length(aabb_center - camera.position.xyz) - 0.5 * aabb_rough_extent, 0.0);

                let fov90_distance_to_screen_ratio = 2.0f;
                let pixel_size_at_1m = fov90_distance_to_screen_ratio / max(camera.resolution.x, camera.resolution.y);
                let aabb_size_at_1m = (aabb_rough_extent / aabb_rough_camera_distance);
                let rough_aabb_pixel_size = aabb_size_at_1m / pixel_size_at_1m;

                for (var i = 1; i < mesh.lod_count; i++) {
                    let mesh_lod = mesh.lods[i];
                    let rough_pixel_error = rough_aabb_pixel_size * mesh_lod.error;
                    if (rough_pixel_error < camera.acceptable_lod_error) {
                        lod_index = i;
                    } else {
                        break;
                    }
                }
            }

            meshlet_count = mesh.lods[lod_index].meshlet_count;
        }
    }

    let is_blend = HAS_FLAG(CULL_FLAGS, CullFlag.RouteBlend) && render_class == MaterialClass::Blend;
    let is_refractor = HAS_FLAG(CULL_FLAGS, CullFlag.RouteRefractors) && render_class == MaterialClass::Refractor;
    let blend_count = select(is_blend, meshlet_count, 0u);
    let refractor_count = select(is_refractor, meshlet_count, 0u);
    let opaque_count = select(is_blend || is_refractor, 0u, meshlet_count);

    let lane_opaque_offset = WavePrefixSum(opaque_count);
    let total_opaque_count = WaveActiveSum(opaque_count);
    let lane_blend_offset = WavePrefixSum(blend_count);
    let total_blend_count = WaveActiveSum(blend_count);
    let lane_refractor_offset = WavePrefixSum(refractor_count);
    let total_refractor_count = WaveActiveSum(refractor_count);

    var opaque_base = 0u;
    var blend_base = 0u;
    var refractor_base = 0u;
    if (WaveIsFirstLane()) {
        opaque_base = __atomic_add(visibility[0].total_visible_meshlet_instances, total_opaque_count, MemoryOrder::Relaxed);
        let new_total = opaque_base + total_opaque_count;
        let needed_groups = (new_total + CULLING_MESHLET_COUNT - 1u) / CULLING_MESHLET_COUNT;
        __atomic_max(cull_meshlets_cmd[0].x, needed_groups, MemoryOrder::Relaxed);

        if (HAS_FLAG(CULL_FLAGS, CullFlag.RouteBlend)) {
            blend_base = __atomic_add(visibility[0].total_blend_meshlet_instances, total_blend_count, MemoryOrder::Relaxed);
            let new_blend_total = blend_base + total_blend_count;
            let blend_groups = (new_blend_total + CULLING_MESHLET_COUNT - 1u) / CULLING_MESHLET_COUNT;
            __atomic_max(blend_cull_meshlets_cmd[0].x, blend_groups, MemoryOrder::Relaxed);
        }

        if (HAS_FLAG(CULL_FLAGS, CullFlag.RouteRefractors)) {
            refractor_base = __atomic_add(visibility[0].total_refractor_meshlet_instances, total_refractor_count, MemoryOrder::Relaxed);
            let new_refractor_total = refractor_base + total_refractor_count;
            let refractor_groups = (new_refractor_total + CULLING_MESHLET_COUNT - 1u) / CULLING_MESHLET_COUNT;
            __atomic_max(refractor_cull_meshlets_cmd[0].x, refractor_groups, MemoryOrder::Relaxed);
        }
    }
    opaque_base = WaveReadLaneFirst(opaque_base);
    blend_base = WaveReadLaneFirst(blend_base);
    refractor_base = WaveReadLaneFirst(refractor_base);

    if (meshlet_count > 0u) {
        mesh_instances[mesh_instance_index].lod_index = lod_index;
    }

    for (u32 i = 0u; i < opaque_count; i++) {
        let offset = opaque_base + lane_opaque_offset + i;
        meshlet_instances[offset].mesh_instance_index = mesh_instance_index;
        meshlet_instances[offset].meshlet_index = i;
    }

    for (u32 i = 0u; i < blend_count; i++) {
        let offset = blend_base + lane_blend_offset + i;
        transparent_meshlet_instances[offset].mesh_instance_index = mesh_instance_index;
        transparent_meshlet_instances[offset].meshlet_index = i;
    }

    for (u32 i = 0u; i < refractor_count; i++) {
        let offset = camera.meshlet_instance_capacity - 1u - (refractor_base + lane_refractor_offset + i);
        transparent_meshlet_instances[offset].mesh_instance_index = mesh_instance_index;
        transparent_meshlet_instances[offset].meshlet_index = i;
    }
}
