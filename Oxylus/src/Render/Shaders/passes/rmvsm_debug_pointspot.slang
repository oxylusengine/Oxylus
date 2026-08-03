import common;
import gpu;
import scene;
import rmvsm;

#include <fullscreen.slang>
#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(2)]] StructuredBuffer<u32x4> light_grid;
[[vk::binding(3)]] Texture2D<f32> depth_image;
[[vk::binding(4)]] Texture2DArray<u32> vsm_page_table;

func hsv_to_rgb(f32x3 hsv) -> f32x3 {
    let n = f32x3(5.0, 3.0, 1.0);
    let k = (n + hsv.x / 1.0471975512) % 6.0;
    return hsv.z - hsv.z * hsv.y * max(f32x3(0.0), min(k, min(4.0 - k, f32x3(1.0))));
}

func first_shadow_light(u32x2 point_mask, u32x2 spot_mask, out i32 layer, out i32 face) -> bool {
    for (var word = 0; word < 2; word++) {
        if (point_mask[word] != 0u) {
            let slot = word * 32 + i32(firstbitlow(point_mask[word]));
            layer = slot * 6;
            face = -1; // resolved by caller from direction
            return true;
        }
    }
    for (var word = 0; word < 2; word++) {
        if (spot_mask[word] != 0u) {
            let slot = word * 32 + i32(firstbitlow(spot_mask[word]));
            layer = vsm_spot_layer(slot);
            face = -2; // spot
            return true;
        }
    }
    return false;
}

[[shader("fragment")]]
func fs_main(
    VertexOutput input,
    uniform u32 mip_bias_min,
    uniform f32x3 light_grid_origin
) -> f32x4 {
    let pixel_pos = i32x3(i32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);
    if (depth == 0.0) {
        return 1.0;
    }

    let world_position = camera.unproject_uv(input.tex_coord, depth);

    let grid_cell = light_grid_cell_coords(world_position, light_grid_origin);
    if (!light_grid_cell_in_bounds(grid_cell)) {
        return f32x4(0.05, 0.05, 0.05, 1.0);
    }
    let cell_mask = light_grid[light_grid_cell_index(grid_cell)];

    i32 layer = 0;
    i32 face_tag = 0;
    if (!first_shadow_light(cell_mask.xy, cell_mask.zw, layer, face_tag)) {
        // No shadow-casting point/spot light influences this pixel.
        return f32x4(0.1, 0.1, 0.1, 1.0);
    }

    let is_point = face_tag == -1;
    if (is_point) {
        let base_view = views[layer];
        let face = vsm_point_face_index(world_position - base_view.light_position);
        layer = layer + face;
    }

    let view = views[layer];
    if (view.range <= 0.0) {
        return f32x4(0.1, 0.1, 0.1, 1.0);
    }

    let proj = view.project(world_position);
    if (!proj.valid) {
        return f32x4(0.1, 0.1, 0.1, 1.0);
    }

    // Footprint corners for mip selection, matching marking/shading.
    let uv_offset = (1.0 / camera.resolution) * 0.5;
    let wp_left = camera.unproject_uv(input.tex_coord + f32x2(-uv_offset.x, uv_offset.y), depth);
    let wp_right = camera.unproject_uv(input.tex_coord + f32x2(uv_offset.x, uv_offset.y), depth);
    let mip = vsm_pointspot_mip(view.project(wp_left).uv, view.project(wp_right).uv, mip_bias_min);

    let mip_virtual_size = VSM_POINT_SPOT_VIRTUAL_SIZE >> mip;
    let virtual_texel = i32x2(clamp(proj.uv, 0.0, 0.99999) * f32(mip_virtual_size));
    let page_coords = virtual_texel / VSM_PAGE_SIZE;
    let in_page_texel = virtual_texel % VSM_PAGE_SIZE;

    let page = VSMPageMetadata(vsm_page_table.Load(i32x4(page_coords, layer, mip)));

    // Page-border highlighting, hue per mip (the distance-selected LOD), matching
    // the directional debug view which hues by clipmap. mip 0 (nearest) .. mip 5.
    let near_border = any(in_page_texel < 2) || any(in_page_texel > VSM_PAGE_SIZE - 2);
    if (near_border) {
        let hue = f32(mip) / f32(VSM_POINT_SPOT_MIP_COUNT) * TAU;
        return f32x4(hsv_to_rgb(f32x3(hue, 1.0, 0.5)), 1.0);
    }

    if (page.is_visible() && !page.is_backed()) {
        // Requested but unbacked (allocation pending or failed).
        return f32x4(1.0, 0.0, 0.0, 1.0);
    }

    // R = backed, G = dirty, B = visible.
    let status = f32x3(f32(page.is_backed()), f32(page.is_dirty()), f32(page.is_visible()));
    return f32x4(status, 1.0);
}
