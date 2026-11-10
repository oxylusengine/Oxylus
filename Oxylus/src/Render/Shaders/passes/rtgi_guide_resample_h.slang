import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> pioneer_hit_y_image;
[[vk::binding(3, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA32F> h_resample_hit_y_image;
[[vk::binding(4, 0)]] StorageImage2D<f32x2, ImageFormat::RG32F> h_resample_weight_image;

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
        h_resample_hit_y_image[cell] = f32x4(0.0);
        h_resample_weight_image[cell] = f32x2(0.0);
        return;
    }

    let center_depth = half_res_depth_image.Load(i32x3(i32x2(center_pixel), 0));
    if (center_depth == 0.0) {
        h_resample_hit_y_image[cell] = f32x4(0.0);
        h_resample_weight_image[cell] = f32x2(0.0);
        return;
    }

    let center_uv = (f32x2(center_pixel) + 0.5) / half_size;
    let center_position = camera.world_position_from_linear_depth(center_uv, center_depth);
    let center_pixel_width = rtgi_pixel_width_ws(camera, center_uv, center_depth);

    com::rand_seed(cell.x * 9629u + cell.y * 10069u + settings.frame_index * 257u + 11u);

    let tap_count = max(1, i32(RTGI_GUIDE_RESAMPLE_WINDOW) / i32(RTGI_PIONEER_GRID_DIV));
    let base_x = i32(cell.x) - (tap_count / 2) * RTGI_GUIDE_RESAMPLE_STRIDE;
    var weight_sum = 0.0;
    var valid_count = 0.0;
    var held_hit_y = f32x4(0.0);

    for (i32 i = 0; i < tap_count; i++) {
        let cell_x = base_x + i * RTGI_GUIDE_RESAMPLE_STRIDE;
        if (or(cell_x < 0, cell_x >= i32(grid_size.x))) {
            continue;
        }

        let candidate_cell = u32x2(u32(cell_x), cell.y);
        let candidate_hit_y = pioneer_hit_y_image.Load(i32x3(i32x2(candidate_cell), 0));
        if (candidate_hit_y.w < 0.0) {
            continue;
        }

        let candidate_pixel = rtgi_pioneer_pixel(candidate_cell, settings.frame_index);
        let candidate_load = i32x3(i32x2(candidate_pixel), 0);
        let candidate_depth = half_res_depth_image.Load(candidate_load);
        let candidate_uv = (f32x2(candidate_pixel) + 0.5) / half_size;
        let candidate_position = camera.world_position_from_linear_depth(candidate_uv, candidate_depth);
        let candidate_pixel_width = rtgi_pixel_width_ws(camera, candidate_uv, candidate_depth);

        let is_valid = distance(center_position, candidate_position) <=
                       RTGI_GUIDE_RESAMPLE_PX_DIST_THRESHOLD * min(center_pixel_width, candidate_pixel_width);

        // log keeps one very bright hit from owning the pick across the whole window
        let importance = select(is_valid, max(log(candidate_hit_y.w + 1.0), RTGI_GUIDE_IMPORTANCE_FLOOR), 0.0);
        weight_sum += importance;
        valid_count += f32(is_valid);
        // a zero importance never passes, rand() * weight_sum is never negative
        let is_selected = com::rand() * weight_sum < importance;
        held_hit_y = select(is_selected, candidate_hit_y, held_hit_y);
    }

    h_resample_hit_y_image[cell] = held_hit_y;
    h_resample_weight_image[cell] = f32x2(weight_sum, valid_count);
}
