import common;
import gpu;
import scene;
import cull;
import rmvsm;

#include <defines.slang>
#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(3)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(4)]] StructuredBuffer<VSMMeshletInstance> vsm_meshlet_instances;
[[vk::binding(5)]] StructuredBuffer<u32> vsm_meshlet_instance_count;
[[vk::binding(6)]] Texture2DArray<u32> page_tables;
[[vk::binding(7)]] RWStructuredBuffer<u32> visible_meshlet_instances_indices;
[[vk::binding(8)]] RWStructuredBuffer<DispatchIndirectCommand> cull_triangles_cmd;

groupshared u32 visible_count_shared;
groupshared u32 base_index_shared;
groupshared u32 meshlet_indices_shared[CULLING_MESHLET_COUNT];

func test_pointspot_pages(in ScreenAabb screen_aabb, u32 layer, i32 mip) -> bool {
    let mip_table_size = VSM_POINT_SPOT_PAGE_TABLE_SIZE >> mip;
    let min_page = clamp(i32x2(screen_aabb.min.xy * f32(mip_table_size)), 0, mip_table_size - 1);
    let max_page = clamp(i32x2(screen_aabb.max.xy * f32(mip_table_size)), 0, mip_table_size - 1);

    let page_area = (max_page.x - min_page.x + 1) * (max_page.y - min_page.y + 1);
    if (page_area > 64) {
        return true;
    }

    for (var y = min_page.y; y <= max_page.y; y++) {
        for (var x = min_page.x; x <= max_page.x; x++) {
            let page = VSMPageMetadata(page_tables.Load(i32x4(x, y, layer, mip)));
            if (page.is_backed() && page.is_dirty()) {
                return true;
            }
        }
    }

    return false;
}

[[shader("compute")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func cs_main(
    u32 global_thread_id : SV_DispatchThreadID,
    u32 group_thread_id : SV_GroupThreadID,
    uniform VSMPointSpotContext ctx
) -> void {
    if (group_thread_id == 0) {
        visible_count_shared = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    var local_slot = ~0u;
    let instance_count = min(vsm_meshlet_instance_count[0], VSM_POINT_SPOT_MAX_MESHLET_INSTANCES);
    if (global_thread_id < instance_count) {
        let vsm_meshlet_instance = vsm_meshlet_instances[global_thread_id];
        let mesh_instance = mesh_instances[vsm_meshlet_instance.mesh_instance_index];
        let transform = transforms[mesh_instance.transform_index];
        let view = views[vsm_meshlet_instance.layer];
        let mvp = mul(view.projection_view, transform.world);

        let mesh = meshes[mesh_instance.mesh_index];
        let mesh_lod = mesh.lods[mesh_instance.lod_index];
        let meshlet_bounds = mesh_lod.meshlet_bounds[vsm_meshlet_instance.meshlet_index];
        let aabb_center = meshlet_bounds.get_aabb_center();
        let aabb_extent = meshlet_bounds.get_aabb_extent();
        let cone_cutoff = meshlet_bounds.get_cone_cutoff();

        let cone_axis = normalize(transform.to_world_normal(meshlet_bounds.get_cone_axis()));
        let world_center = transform.to_world_position(aabb_center).xyz;
        let world_radius = transform.to_world_radius(length(aabb_extent * 0.5));
        let in_range = distance(world_center, view.light_position) - world_radius <= view.range;
        let cone_visible = cone_cutoff >= 1.0 ||
                           !test_cone(world_center, world_radius, cone_axis, cone_cutoff, view.light_position);

        if (in_range && cone_visible && test_frustum(mvp, aabb_center, aabb_extent)) {
            var visible = true;
            if (let screen_aabb = project_aabb(mvp, view.z_near, aabb_center, aabb_extent)) {
                visible = test_pointspot_pages(screen_aabb, vsm_meshlet_instance.layer, ctx.curr_mip);
            }

            if (visible) {
                local_slot = __atomic_add(visible_count_shared, 1, MemoryOrder::Relaxed);
                meshlet_indices_shared[local_slot] = global_thread_id;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (visible_count_shared > 0) {
        if (group_thread_id == 0) {
            base_index_shared = __atomic_add(cull_triangles_cmd[0].x, visible_count_shared, MemoryOrder::Relaxed);
        }
        GroupMemoryBarrierWithGroupSync();

        if (local_slot < visible_count_shared) {
            let slot = base_index_shared + local_slot;
            if (slot < VSM_POINT_SPOT_MAX_MESHLET_INSTANCES) {
                visible_meshlet_instances_indices[slot] = meshlet_indices_shared[local_slot];
            }
        }
    }
}
