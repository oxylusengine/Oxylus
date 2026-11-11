import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> h_resample_hit_y_image;
[[vk::binding(3, 0)]] Texture2D<f32x2> h_resample_weight_image;
[[vk::binding(4, 0)]] RWTexture2D<f32x4> pioneer_guide_hit_y_image;

[[shader("compute")]]
[numthreads(16, 16, 1)]
func cs_main(u32x2 cell : SV_DispatchThreadID, uniform RTGISettings settings) {
    let grid_size = rtgi_pioneer_grid_size(settings.half_size);
    if (any(cell >= grid_size)) {
        return;
    }

    let half_size = f32x2(settings.half_size);
    let center_pixel = rtgi_pioneer_pixel(cell, settings.frame_index);
    if (any(center_pixel >= settings.half_size)) {
        pioneer_guide_hit_y_image[cell] = f32x4(0.0);
        return;
    }

    let center_depth = half_res_depth_image.Load(i32x3(i32x2(center_pixel), 0));
    if (center_depth == 0.0) {
        pioneer_guide_hit_y_image[cell] = f32x4(0.0);
        return;
    }

    let center_uv = (f32x2(center_pixel) + 0.5) / half_size;
    let center_position = camera.world_position_from_linear_depth(center_uv, center_depth);
    let center_pixel_width = rtgi_pixel_width_ws(camera, center_uv, center_depth);

    com::rand_seed(cell.x * 9629u + cell.y * 10069u + settings.frame_index * 257u + 29u);

    let tap_count = max(1, i32(RTGI_GUIDE_RESAMPLE_WINDOW) / i32(RTGI_PIONEER_GRID_DIV));
    let base_y = i32(cell.y) - (tap_count / 2) * RTGI_GUIDE_RESAMPLE_STRIDE;
    var weight_sum = 0.0;
    var held_cell = cell;
    var is_held = false;

    for (i32 i = 0; i < tap_count; i++) {
        let row = base_y + i * RTGI_GUIDE_RESAMPLE_STRIDE;
        if (or(row < 0, row >= i32(grid_size.y))) {
            continue;
        }

        let row_cell = u32x2(cell.x, u32(row));
        let row_weight = h_resample_weight_image.Load(i32x3(i32x2(row_cell), 0));
        if (row_weight.x <= 0.0) {
            continue;
        }

        let row_pixel = rtgi_pioneer_pixel(row_cell, settings.frame_index);
        if (any(row_pixel >= settings.half_size)) {
            continue;
        }

        let row_depth = half_res_depth_image.Load(i32x3(i32x2(row_pixel), 0));
        let row_uv = (f32x2(row_pixel) + 0.5) / half_size;
        let row_position = camera.world_position_from_linear_depth(row_uv, row_depth);
        let row_pixel_width = rtgi_pixel_width_ws(camera, row_uv, row_depth);

        let is_valid = and(
            row_depth != 0.0,
            distance(center_position, row_position) <=
                RTGI_GUIDE_RESAMPLE_PX_DIST_THRESHOLD * min(center_pixel_width, row_pixel_width)
        );

        let tap_weight = select(is_valid, row_weight.x, 0.0);
        weight_sum += tap_weight;
        // a zero weight never passes, rand() * weight_sum is never negative
        let is_selected = com::rand() * weight_sum < tap_weight;
        held_cell = select(is_selected, row_cell, held_cell);
        is_held = or(is_held, is_selected);
    }

    // the held row is read once after the loop, it is the same texel the reservoir picked
    let held_hit_y = h_resample_hit_y_image.Load(i32x3(i32x2(held_cell), 0));
    pioneer_guide_hit_y_image[cell] = select(is_held, held_hit_y, f32x4(0.0));
}
