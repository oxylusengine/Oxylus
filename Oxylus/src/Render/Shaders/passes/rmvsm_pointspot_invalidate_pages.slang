import common;
import gpu;
import rmvsm;
import scene;
import cull;

#include <defines.slang>
#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip0;
[[vk::binding(1)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip1;
[[vk::binding(2)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip2;
[[vk::binding(3)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip3;
[[vk::binding(4)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip4;
[[vk::binding(5)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip5;

[[vk::binding(6)]] StructuredBuffer<u32> dirty_mesh_instance_indices;
[[vk::binding(7)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(8)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(9)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(10)]] StructuredBuffer<TransformPrevious> transforms_previous;
[[vk::binding(11)]] StructuredBuffer<VSMPointSpotView> views;

constexpr static i32 INVALIDATE_SECTIONS_PER_DIM = 4;

func invalidate_page(i32x3 address, i32 mip) -> void {
    var page_data = 0u;
    switch (mip) {
        case 0: page_data = page_tables_mip0.Load(address); break;
        case 1: page_data = page_tables_mip1.Load(address); break;
        case 2: page_data = page_tables_mip2.Load(address); break;
        case 3: page_data = page_tables_mip3.Load(address); break;
        case 4: page_data = page_tables_mip4.Load(address); break;
        default: page_data = page_tables_mip5.Load(address); break;
    }

    var page = VSMPageMetadata(page_data);
    if (!page.is_backed()) {
        return;
    }
    page.reset();
    page.set_invalidated(true);

    switch (mip) {
        case 0: page_tables_mip0.Store(address, page.data); break;
        case 1: page_tables_mip1.Store(address, page.data); break;
        case 2: page_tables_mip2.Store(address, page.data); break;
        case 3: page_tables_mip3.Store(address, page.data); break;
        case 4: page_tables_mip4.Store(address, page.data); break;
        default: page_tables_mip5.Store(address, page.data); break;
    }
}

func invalidate_in_region(
    f32x4x4 mvp,
    f32 near,
    f32x3 aabb_center,
    f32x3 aabb_extent,
    i32x2 region_min,
    i32x2 region_max,
    i32 layer,
    i32 mip,
    i32 mip_table_size
) -> void {
    var page_min = region_min;
    var page_max = region_max;

    if (let screen_aabb = project_aabb(mvp, near, aabb_center, aabb_extent)) {
        let uv_min = clamp(screen_aabb.min.xy, 0.0, 1.0);
        let uv_max = clamp(screen_aabb.max.xy, 0.0, 1.0);
        if (any(uv_min >= uv_max)) {
            return;
        }

        let limits = i32x4(i32x2(0), i32x2(mip_table_size - 1));
        page_min = max(clamp(i32x2(floor(uv_min * f32(mip_table_size))), limits.xy, limits.zw), region_min);
        page_max = min(clamp(i32x2(ceil(uv_max * f32(mip_table_size))) - 1, limits.xy, limits.zw), region_max);
        if (any(page_min > page_max)) {
            return;
        }
    }

    for (var y = page_min.y; y <= page_max.y; y++) {
        for (var x = page_min.x; x <= page_max.x; x++) {
            invalidate_page(i32x3(x, y, layer), mip);
        }
    }
}

[[shader("compute")]]
[[numthreads(1, INVALIDATE_PAGES_WORKGROUP_Y, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform VSMPointSpotContext ctx
) -> void {
    let mesh_index = thread_id.x;
    let section_index = i32(thread_id.y);
    let layer = i32(thread_id.z);

    if (section_index >= INVALIDATE_SECTIONS_PER_DIM * INVALIDATE_SECTIONS_PER_DIM || layer >= i32(ctx.layer_count)) {
        return;
    }

    let view = views[layer];
    if (view.range <= 0.0) {
        return;
    }

    let section_coords = i32x2(
        section_index % INVALIDATE_SECTIONS_PER_DIM,
        section_index / INVALIDATE_SECTIONS_PER_DIM
    );

    let mesh_instance = mesh_instances[dirty_mesh_instance_indices[mesh_index]];
    let transform = transforms[mesh_instance.transform_index];
    let transform_previous = transforms_previous[mesh_instance.transform_index];
    let mesh = meshes[mesh_instance.mesh_index];

    let world_center_curr = transform.to_world_position(mesh.bounds.aabb_center).xyz;
    let world_radius_curr = transform.to_world_radius(length(mesh.bounds.aabb_extent * 0.5));
    let prev_world = TransformWorld(transform_previous.previous_world);
    let world_center_prev = prev_world.to_world_position(mesh.bounds.aabb_center).xyz;
    let world_radius_prev = prev_world.to_world_radius(length(mesh.bounds.aabb_extent * 0.5));
    let in_range_curr = distance(world_center_curr, view.light_position) - world_radius_curr <= view.range;
    let in_range_prev = distance(world_center_prev, view.light_position) - world_radius_prev <= view.range;
    if (!in_range_curr && !in_range_prev) {
        return;
    }

    let mvp_prev = mul(view.projection_view, transform_previous.previous_world);
    let mvp_curr = mul(view.projection_view, transform.world);

    for (var mip = 0; mip < VSM_POINT_SPOT_MIP_COUNT; mip++) {
        let mip_table_size = VSM_POINT_SPOT_PAGE_TABLE_SIZE >> mip;
        let block_size = max(mip_table_size / INVALIDATE_SECTIONS_PER_DIM, 1);
        let region_min = section_coords * block_size;
        if (any(region_min >= mip_table_size)) {
            continue;
        }
        let region_max = min(region_min + block_size, mip_table_size) - 1;

        if (in_range_prev) {
            invalidate_in_region(
                mvp_prev, view.z_near,
                mesh.bounds.aabb_center, mesh.bounds.aabb_extent,
                region_min, region_max, layer, mip, mip_table_size
            );
        }
        if (in_range_curr) {
            invalidate_in_region(
                mvp_curr, view.z_near,
                mesh.bounds.aabb_center, mesh.bounds.aabb_extent,
                region_min, region_max, layer, mip, mip_table_size
            );
        }
    }
}
