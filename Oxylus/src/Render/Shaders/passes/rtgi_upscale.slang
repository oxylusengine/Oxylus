import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32x4> half_res_diffuse_image;
[[vk::binding(2, 0)]] Texture2D<f32x2> half_res_diffuse2_image;
[[vk::binding(3, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(4, 0)]] Texture2D<u32> half_res_face_normal_image;
[[vk::binding(5, 0)]] Texture2D<f32> depth_image;
[[vk::binding(6, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(7, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> resolved_diffuse_image;

static const i32 UPSCALE_X = 16;
static const i32 GS_PRELOAD_WIDTH = UPSCALE_X / 2 + 2;
static const f32 WIDE_FOOTPRINT_SIGMA = 1.0;

groupshared f32x4 gs_half_diffuse[GS_PRELOAD_WIDTH][GS_PRELOAD_WIDTH];
groupshared f32x2 gs_half_diffuse2[GS_PRELOAD_WIDTH][GS_PRELOAD_WIDTH];
groupshared f32x3 gs_half_normals[GS_PRELOAD_WIDTH][GS_PRELOAD_WIDTH];
groupshared f32x4 gs_half_positions[GS_PRELOAD_WIDTH][GS_PRELOAD_WIDTH];

[[shader("compute")]]
[numthreads(16, 16, 1)]
func cs_main(
    u32x2 dtid : SV_DispatchThreadID,
    i32x2 group_id : SV_GroupID,
    i32x2 in_group_id : SV_GroupThreadID,
    uniform u32x2 render_size
) {
    let full_size = i32x2(render_size);
    let half_size = (full_size + 1) / 2;
    let full_pixel = min(i32x2(dtid), full_size - 1);

    let group_base_half = group_id * (UPSCALE_X / 2) - 1;
    if (all(in_group_id < GS_PRELOAD_WIDTH)) {
        let load_index = clamp(in_group_id + group_base_half, i32x2(0), half_size - 1);
        let load = i32x3(load_index, 0);
        let depth = half_res_depth_image.Load(load);
        gs_half_diffuse[in_group_id.x][in_group_id.y] = half_res_diffuse_image.Load(load);
        gs_half_diffuse2[in_group_id.x][in_group_id.y] = half_res_diffuse2_image.Load(load);
        gs_half_normals[in_group_id.x][in_group_id.y] = com::decompress_normal_32(half_res_face_normal_image.Load(load));

        let sample_uv = (f32x2(load_index) + 0.5) / f32x2(half_size);
        gs_half_positions[in_group_id.x][in_group_id.y] = f32x4(camera.world_position_from_linear_depth(sample_uv, depth), depth);
    }

    GroupMemoryBarrierWithGroupSync();

    if (any(i32x2(dtid) >= full_size)) {
        return;
    }

    let pixel_depth = depth_image.Load(i32x3(full_pixel, 0));
    if (pixel_depth == 0.0) {
        resolved_diffuse_image[dtid] = f32x4(0.0);
        return;
    }

    let normals = normal_image.Load(i32x3(full_pixel, 0));
    let pixel_face_normal = normalize(com::oct_to_vec3(normals.ba));
    let pixel_detail_normal = normalize(com::oct_to_vec3(normals.rg));

    let uv = (f32x2(full_pixel) + 0.5) / f32x2(full_size);
    let linear_depth = camera.linear_depth(pixel_depth);
    let position = camera.world_position_from_linear_depth(uv, linear_depth);
    let rcp_pixel_width_ws = 1.0 / rtgi_pixel_width_ws(camera, uv, linear_depth);

    // distances in half res pixels from a left or top sub pixel centre to the taps at -1, 0 and +1
    let tap_distances_left = f32x3(0.75, 0.25, 1.25);
    let wide_weights = exp(-(tap_distances_left * tap_distances_left) / (2.0 * WIDE_FOOTPRINT_SIGMA * WIDE_FOOTPRINT_SIGMA));
    let weights_left = wide_weights / dot(wide_weights, f32x3(1.0));
    let subpixel = full_pixel & 1;
    let weights_x = select(subpixel.x == 0, weights_left, weights_left.zyx);
    let weights_y = select(subpixel.y == 0, weights_left, weights_left.zyx);

    var acc_diffuse = f32x4(0.0);
    var acc_diffuse2 = f32x2(0.0);
    var acc_weight = 0.0;
    var fallback_diffuse = f32x4(0.0);
    var fallback_diffuse2 = f32x2(0.0);
    var fallback_weight = 0.0;
    for (i32 col = 0; col < 3; col++) {
        for (i32 row = 0; row < 3; row++) {
            let offset = i32x2(row - 1, col - 1);
            let gs_index = in_group_id / 2 + offset + 1;
            let sample_position_depth = gs_half_positions[gs_index.x][gs_index.y];
            let sample_sh_y = gs_half_diffuse[gs_index.x][gs_index.y];
            let sample_cocg = gs_half_diffuse2[gs_index.x][gs_index.y];
            let sample_normal = gs_half_normals[gs_index.x][gs_index.y];
            let sample_position = sample_position_depth.xyz;

            // a sky tap holds finite data, so a zero weight drops it
            let is_surface = sample_position_depth.w != 0.0;
            let tent_weight = select(is_surface, weights_x[row] * weights_y[col], 0.0);
            let plane_distance_px = abs(com::plane_distance(position, pixel_face_normal, sample_position)) * rcp_pixel_width_ws;
            let geometry_weight = f32(plane_distance_px < 3.0);
            let facing = max(0.0, dot(sample_normal, pixel_face_normal));
            let normal_weight = facing * facing * facing * facing;
            let weight = tent_weight * geometry_weight * normal_weight;
            acc_diffuse += weight * sample_sh_y;
            acc_diffuse2 += weight * sample_cocg;
            acc_weight += weight;

            let offset_to_sample = position - sample_position;
            let distance_squared = dot(offset_to_sample, offset_to_sample);
            let lenient_weight = tent_weight / (1.0 + distance_squared * distance_squared) * (0.1 + facing);
            fallback_diffuse += lenient_weight * sample_sh_y;
            fallback_diffuse2 += lenient_weight * sample_cocg;
            fallback_weight += lenient_weight;
        }
    }

    let is_geometry_match = acc_weight > 1e-5;
    let sh_y = select(is_geometry_match, acc_diffuse / (acc_weight + 1e-7), fallback_diffuse / (fallback_weight + 1e-7));
    let co_cg = select(is_geometry_match, acc_diffuse2 / (acc_weight + 1e-7), fallback_diffuse2 / (fallback_weight + 1e-7));

    let is_covered = fallback_weight > 0.0;
    resolved_diffuse_image[dtid] = select(
        is_covered,
        f32x4(rtgi_sh_resolve_diffuse(sh_y, co_cg, pixel_detail_normal), 1.0),
        f32x4(0.0)
    );
}
