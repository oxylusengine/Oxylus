import rmvsm;
import common;
import cull;

[[vk::constant_id(50)]] const i32 VSM_POINT_SPOT_VIRTUAL_SIZE = 4096;
[[vk::constant_id(51)]] const i32 VSM_POINT_SPOT_PAGE_TABLE_SIZE = 32;
[[vk::constant_id(52)]] const i32 VSM_POINT_SPOT_MIP_COUNT = 6;
[[vk::constant_id(53)]] const i32 VSM_PAGE_SIZE = 128;
[[vk::constant_id(54)]] const i32 VSM_PHYSICAL_PAGE_TABLE_SIZE = 8192;

func test_pointspot_pages(in ScreenAabb screen_aabb, Texture2DArray<u8> hpb, u32 layer, i32 mip) -> bool {
    let mip_table_size = VSM_POINT_SPOT_PAGE_TABLE_SIZE >> mip;
    let min_page = clamp(i32x2(screen_aabb.min.xy * f32(mip_table_size)), 0, mip_table_size - 1);
    let max_page = clamp(i32x2(screen_aabb.max.xy * f32(mip_table_size)), 0, mip_table_size - 1);

    // Climb until the projected bounds span at most two texels per axis, keeping
    // the occupancy walk bounded at 3x3 even for very large meshlets.
    let page_span = max(max_page.x - min_page.x, max_page.y - min_page.y) + 1;
    let level = max(i32(ceil(log2(f32(page_span)))) - 1, 0);

    let level_min = min_page >> level;
    let level_max = max_page >> level;
    for (var y = level_min.y; y <= level_max.y; y++) {
        for (var x = level_min.x; x <= level_max.x; x++) {
            if (hpb.Load(i32x4(x, y, i32(layer), level)) != 0) {
                return true;
            }
        }
    }

    return false;
}

func vsm_pointspot_mip(f32x2 uv_a, f32x2 uv_b, u32 mip_bias_min) -> i32 {
    let texel_dist = distance(uv_a, uv_b) * f32(VSM_POINT_SPOT_VIRTUAL_SIZE);
    let mip = max(i32(log2(max(texel_dist, 1.0))), 0);
    return clamp(mip, i32(mip_bias_min), VSM_POINT_SPOT_MIP_COUNT - 1);
}

func vsm_pointspot_page_coords(f32x2 uv, i32 mip) -> i32x2 {
    let clamped_uv = clamp(uv, 0.0, 0.99999);
    return i32x2(clamped_uv * f32(VSM_POINT_SPOT_PAGE_TABLE_SIZE >> mip));
}

func vsm_pointspot_load_depth(
    Texture2DArray<u32> page_tables,
    Texture2D<f32> physical_pages,
    i32 layer,
    i32 mip,
    f32x2 uv
) -> f32 {
    let clamped_uv = clamp(uv, 0.0, 0.99999);
    for (var m = mip; m < VSM_POINT_SPOT_MIP_COUNT; m++) {
        let page_coords = i32x2(clamped_uv * f32(VSM_POINT_SPOT_PAGE_TABLE_SIZE >> m));
        let page = VSMPageMetadata(page_tables.Load(i32x4(page_coords, layer, m)));
        if (page.is_backed()) {
            let physical_page_coords = page.get_physical_coords(VSM_PHYSICAL_PAGE_TABLE_SIZE, VSM_PAGE_SIZE);
            let virtual_texel_coord = i32x2(clamped_uv * f32(VSM_POINT_SPOT_VIRTUAL_SIZE >> m));
            let in_page_texel_coord = virtual_texel_coord % VSM_PAGE_SIZE;
            let physical_texel_coord = i32x2(physical_page_coords) * VSM_PAGE_SIZE + in_page_texel_coord;
            return physical_pages.Load(i32x3(physical_texel_coord, 0));
        }
    }

    return 1.0;
}

func vsm_sample_pointspot_shadow(
    in VSMPointSpotView view,
    i32 layer,
    i32 mip,
    Texture2DArray<u32> page_tables,
    Texture2D<f32> physical_pages,
    f32x3 world_pos,
    f32x3 flat_normal,
    f32x3 to_light_dir,
    f32 noise_rotation
) -> f32 {
    let dist = distance(world_pos, view.light_position);

    let texel_world = 2.0 * dist * view.texel_world_scale / f32(VSM_POINT_SPOT_VIRTUAL_SIZE >> mip);
    let n_dot_l = saturate(dot(flat_normal, to_light_dir));
    let receiver_offset = texel_world * (1.0 + 2.0 * (1.0 - n_dot_l));
    let receiver_pos = world_pos + flat_normal * receiver_offset;

    let center = view.project(receiver_pos);
    if (!center.valid) {
        return 1.0;
    }

    let bias_world = max(0.01, texel_world);
    let biased = view.project(receiver_pos + to_light_dir * bias_world);
    let reference_depth = biased.valid ? biased.depth : center.depth;

    let texel_uv = 1.0 / f32(VSM_POINT_SPOT_VIRTUAL_SIZE >> mip);
    let filter_radius_uv = texel_uv * 1.5;

    constexpr static i32 TAP_COUNT = 9;
    var lit = 0.0;
    for (var i = 0; i < TAP_COUNT; i++) {
        var offset = f32x2(0.0);
        if (i > 0) {
            let angle = noise_rotation + f32(i - 1) * (TAU / f32(TAP_COUNT - 1));
            offset = f32x2(cos(angle), sin(angle)) * filter_radius_uv;
        }

        let depth = vsm_pointspot_load_depth(page_tables, physical_pages, layer, mip, center.uv + offset);
        lit += depth < reference_depth ? 0.0 : 1.0;
    }

    return lit / f32(TAP_COUNT);
}
