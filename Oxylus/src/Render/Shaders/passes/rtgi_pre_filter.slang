import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(2, 0)]] Texture2D<u32> half_res_face_normal_image;
[[vk::binding(3, 0)]] [[format("r8ui")]] Texture2D<u32> ray_count_image;
[[vk::binding(4, 0)]] Texture2D<u32> pixel_ray_alloc_image;
[[vk::binding(5, 0)]] StructuredBuffer<RTGIRayResult> ray_result;
[[vk::binding(6, 0)]] Texture2D<f32x4> perceptual_rgb_shortness_image;
[[vk::binding(7, 0)]] ConstantBuffer<HistogramLuminance> exposure_buffer;
[[vk::binding(8, 0)]] RWTexture2D<f32x4> pre_filtered_diffuse_image;
[[vk::binding(9, 0)]] RWTexture2D<f32x2> pre_filtered_diffuse2_image;
[[vk::binding(10, 0)]] RWTexture2D<f32> ao_guide_image;
[[vk::binding(11, 0)]] RWTexture2D<f32> perceptual_radiance_image;
[[vk::binding(12, 0)]] RWTexture2D<f32> firefly_factor_image;

static const i32 TILE = 8;
static const i32 QUAD_FILTER_EXTENT = 2;
static const i32 QUAD_FILTER_STRIDE = 1;
static const i32 QUAD_HALO_CELLS = QUAD_FILTER_EXTENT * QUAD_FILTER_STRIDE;
static const i32 TOTAL_FILTER_REACH = QUAD_HALO_CELLS * 2;
static const i32 PRELOAD_SIZE = TILE + TOTAL_FILTER_REACH * 2;
static const i32 CELL_DIM = TILE / 2 + QUAD_HALO_CELLS * 2;
static const i32 CELL_TOTAL = CELL_DIM * CELL_DIM;
static const i32 QUAD_FILTER_DIM = QUAD_FILTER_EXTENT * 2 + 1;
static const f32 FIREFLY_SOFTPLUS_SHARPNESS = 8.0;
static const f32 FIREFLY_SIBLING_RAY_WEIGHT = 1.0;
static const f32x3 CHANNEL_PERCEIVED_BRIGHTNESS = f32x3(0.2973, 1.0, 0.1009);

// world position and depth, and the packed face normal, of the tile plus its filter reach
groupshared f32x4 gs_preload_position_depth[PRELOAD_SIZE][PRELOAD_SIZE];
groupshared u32 gs_preload_normal[PRELOAD_SIZE][PRELOAD_SIZE];

// one entry per 2x2 blur cell, index is cell x then cell y
groupshared f32x3 gs_cell_perceptual_rgb[CELL_DIM][CELL_DIM];
groupshared f32x4 gs_cell_position_depth[CELL_DIM][CELL_DIM];
groupshared f32x3 gs_cell_normal[CELL_DIM][CELL_DIM];
groupshared f32 gs_cell_shortness[CELL_DIM][CELL_DIM];
groupshared f32 gs_cell_ray_count[CELL_DIM][CELL_DIM];
groupshared f32 gs_cell_has_rays[CELL_DIM][CELL_DIM];

// per pixel results exchanged between the quad mates and the paired quads, index is tile x then tile y
groupshared f32x4 gs_own_sh[TILE][TILE];
groupshared f32x2 gs_own_co_cg[TILE][TILE];
groupshared f32 gs_own_factor[TILE][TILE];
groupshared f32x4 gs_filled_sh[TILE][TILE];
groupshared f32x2 gs_filled_co_cg[TILE][TILE];
groupshared f32 gs_filled_factor[TILE][TILE];
groupshared f32 gs_filled_has_value[TILE][TILE];

// closed form (1 + over^k)^(-1/k), about 1 under the ceiling and 1 / over far above it, with a smooth knee
func firefly_softplus_clamp_factor(f32 over, f32 sharpness) -> f32 {
    let log_over = log(max(over, 1.0e-8));
    let soft_min = -com::softplus(-sharpness * log_over) / sharpness;
    return exp(soft_min - log_over);
}

func firefly_ceiling_reference(
    f32x3 sum_geo, f32 weight_geo, f32x3 sum, f32 weight, f32x3 fallback, bool is_tightening
) -> f32x3 {
    let geo = select(weight_geo > 0.0, sum_geo / weight_geo, fallback);
    let non_geo = select(weight > 0.0, sum / weight, fallback);
    let both = min(geo, non_geo);
    let reference_geo = select(weight_geo > 0.0, geo, both);
    let reference_non_geo = select(weight > 0.0, non_geo, both);
    return select(is_tightening, min(reference_geo, reference_non_geo), reference_geo);
}

[[shader("compute")]]
[numthreads(8, 8, 1)]
func cs_main(
    u32x2 group_thread_id : SV_GroupThreadID,
    u32x2 group_id : SV_GroupID,
    uniform RTGISettings settings
) {
    let half_size = f32x2(settings.half_size);
    let max_index = i32x2(settings.half_size) - 1;
    let exposure = rtgi_current_exposure(settings, exposure_buffer.exposure);
    let gtid = i32x2(group_thread_id);
    let group_origin = i32x2(group_id) * TILE;

    for (i32 iter_x = 0; iter_x < (PRELOAD_SIZE + TILE - 1) / TILE; iter_x++) {
        for (i32 iter_y = 0; iter_y < (PRELOAD_SIZE + TILE - 1) / TILE; iter_y++) {
            let in_idx = i32x2(iter_x * TILE + gtid.x, iter_y * TILE + gtid.y);
            if (all(in_idx < i32x2(PRELOAD_SIZE))) {
                let src = clamp(group_origin - TOTAL_FILTER_REACH + in_idx, i32x2(0), max_index);
                let depth = half_res_depth_image.Load(i32x3(src, 0));
                let uv = (f32x2(src) + 0.5) / half_size;
                let position = camera.world_position_from_linear_depth(uv, depth);
                gs_preload_position_depth[in_idx.x][in_idx.y] = f32x4(select(depth != 0.0, position, f32x3(0.0)), depth);
                gs_preload_normal[in_idx.x][in_idx.y] = half_res_face_normal_image.Load(i32x3(src, 0));
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    let dtid = group_origin + gtid;
    let clamped_index = min(dtid, max_index);
    let grad_idx = gtid + TOTAL_FILTER_REACH;
    let center_data = gs_preload_position_depth[grad_idx.x][grad_idx.y];
    let depth = center_data.w;
    let world_position = center_data.xyz;
    let pixel_face_normal = com::decompress_normal_32(gs_preload_normal[grad_idx.x][grad_idx.y]);
    let pixel_uv = (f32x2(clamped_index) + 0.5) / half_size;
    let rcp_pixel_width_ws = 1.0 / max(rtgi_pixel_width_ws(camera, pixel_uv, depth), 1.0e-8);

    // one thread per 2x2 blur cell, the geometry weights use the width of the cell's representative
    for (i32 flat = gtid.y * TILE + gtid.x; flat < CELL_TOTAL; flat += TILE * TILE) {
        let bx = flat % CELL_DIM;
        let by = flat / CELL_DIM;
        let src_base = group_origin + i32x2(bx - QUAD_HALO_CELLS, by - QUAD_HALO_CELLS) * 2;

        var depths = f32x4(0.0);
        var depths_real = f32x4(0.0);
        f32x3 ws_pos[4];
        f32x3 perceptual_rgbs[4];
        f32x3 normals_ws[4];
        f32 ray_shortness[4];
        f32 ray_counts[4];
        for (i32 pi = 0; pi < 4; pi++) {
            let src_idx = clamp(src_base + i32x2(pi & 1, pi >> 1), i32x2(0), max_index);
            let gs_pi = i32x2(bx * 2 + (pi & 1), by * 2 + (pi >> 1));
            let gs_pd = gs_preload_position_depth[gs_pi.x][gs_pi.y];
            ray_counts[pi] = f32(ray_count_image.Load(i32x3(src_idx, 0)));
            depths[pi] = select(ray_counts[pi] > 0.0, gs_pd.w, 0.0);
            depths_real[pi] = gs_pd.w;
            ws_pos[pi] = gs_pd.xyz;
            normals_ws[pi] = com::decompress_normal_32(gs_preload_normal[gs_pi.x][gs_pi.y]);
            let perceptual_geo = perceptual_rgb_shortness_image.Load(i32x3(src_idx, 0));
            perceptual_rgbs[pi] = perceptual_geo.rgb;
            ray_shortness[pi] = perceptual_geo.a;
        }

        let is_any_diffuse = any(depths != 0.0);
        let rep_i = rtgi_quad_downsample_representative(select(is_any_diffuse, depths, depths_real));
        let rep_normal_ws = normals_ws[rep_i];
        let is_rep_surface = depths_real[rep_i] != 0.0;

        let cell_uv = (f32x2(src_base) + 1.0) / half_size;
        let cell_rcp_pixel_width_ws = 1.0 / max(rtgi_pixel_width_ws(camera, cell_uv, depths_real[rep_i]), 1.0e-8);

        var rgb_acc = f32x3(0.0);
        var radiance_weight = 0.0;
        var shortness_acc = 0.0;
        var ray_count_acc = 0.0;
        for (i32 pi = 0; pi < 4; pi++) {
            let gw = select(
                and(is_rep_surface, depths_real[pi] != 0.0),
                rtgi_calc_similar_surface_weight(
                    cell_rcp_pixel_width_ws, ws_pos[rep_i], rep_normal_ws, ws_pos[pi], normals_ws[pi], 4.0
                ),
                0.0
            );
            let diffuse_gw = select(ray_counts[pi] > 0.0, gw, 0.0);
            rgb_acc += perceptual_rgbs[pi] * diffuse_gw;
            radiance_weight += diffuse_gw;
            shortness_acc += ray_shortness[pi] * diffuse_gw;
            ray_count_acc += ray_counts[pi] * diffuse_gw;
        }

        let has_radiance = radiance_weight > 0.0;
        gs_cell_perceptual_rgb[bx][by] = select(has_radiance, rgb_acc / radiance_weight, perceptual_rgbs[rep_i]);
        gs_cell_position_depth[bx][by] = f32x4(ws_pos[rep_i], depths_real[rep_i]);
        gs_cell_normal[bx][by] = rep_normal_ws;
        gs_cell_shortness[bx][by] = select(has_radiance, shortness_acc / radiance_weight, 0.0);
        gs_cell_ray_count[bx][by] = select(has_radiance, ray_count_acc / radiance_weight, 1.0);
        gs_cell_has_rays[bx][by] = f32(has_radiance);
    }

    GroupMemoryBarrierWithGroupSync();

    let pixel_quad_id = gtid / 2;
    let pixel_gs_center = pixel_quad_id + QUAD_HALO_CELLS;

    var perceptual_rgb_acc = f32x3(0.0);
    var geo_weight_acc = 0.0;
    var ring_ray_count_geo_acc = 0.0;
    var ray_shortness_acc = 0.0;
    var ff_perceptual_rgb_sum = f32x3(0.0);
    var ff_weight = 0.0;
    var ff_perceptual_rgb_sum_geo = f32x3(0.0);
    var ff_weight_geo = 0.0;

    // the pixel's own quad is left out of the ring, it is handled per pixel below
    for (i32 qy = -QUAD_FILTER_EXTENT; qy <= QUAD_FILTER_EXTENT; qy++) {
        for (i32 qx = -QUAD_FILTER_EXTENT; qx <= QUAD_FILTER_EXTENT; qx++) {
            let sq = pixel_gs_center + i32x2(qx, qy) * QUAD_FILTER_STRIDE;
            let sq_data = gs_cell_position_depth[sq.x][sq.y];
            let is_ring_surface = and(or(qx != 0, qy != 0), sq_data.w != 0.0);
            let gw = select(
                is_ring_surface,
                rtgi_calc_similar_surface_weight(
                    rcp_pixel_width_ws, world_position, pixel_face_normal, sq_data.xyz, gs_cell_normal[sq.x][sq.y], 4.0
                ),
                0.0
            );

            let diffuse_valid = select(is_ring_surface, gs_cell_has_rays[sq.x][sq.y], 0.0);
            let diffuse_gw = gw * diffuse_valid;
            let perceptual_rgb = gs_cell_perceptual_rgb[sq.x][sq.y];
            perceptual_rgb_acc += perceptual_rgb * diffuse_gw;
            geo_weight_acc += diffuse_gw;
            ring_ray_count_geo_acc += gs_cell_ray_count[sq.x][sq.y] * diffuse_gw;
            ray_shortness_acc += gs_cell_shortness[sq.x][sq.y] * diffuse_gw;

            ff_weight += diffuse_valid;
            ff_perceptual_rgb_sum_geo += perceptual_rgb * diffuse_gw;
            ff_weight_geo += diffuse_gw;
            ff_perceptual_rgb_sum += perceptual_rgb * diffuse_valid;
        }
    }

    // the four pixels of the own quad, tested at full resolution
    var center_surface_weight_acc = 0.0;
    var center_quad_ray_sum_acc = 0.0;
    var imposter_quad_index = -1;
    {
        let center_quad_base = group_origin + pixel_quad_id * 2;
        for (i32 pi = 0; pi < 4; pi++) {
            let tap_idx = clamp(center_quad_base + i32x2(pi & 1, pi >> 1), i32x2(0), max_index);
            let tap_gs = pixel_quad_id * 2 + i32x2(pi & 1, pi >> 1) + TOTAL_FILTER_REACH;
            let tap_data = gs_preload_position_depth[tap_gs.x][tap_gs.y];
            let tap_normal = com::decompress_normal_32(gs_preload_normal[tap_gs.x][tap_gs.y]);
            let tap_gw = select(
                tap_data.w != 0.0,
                rtgi_calc_similar_surface_weight(
                    rcp_pixel_width_ws, world_position, pixel_face_normal, tap_data.xyz, tap_normal, 4.0
                ),
                0.0
            );
            center_surface_weight_acc += tap_gw;

            let tap_ray_count = f32(ray_count_image.Load(i32x3(tap_idx, 0)));
            let tap_perceptual_geo = perceptual_rgb_shortness_image.Load(i32x3(tap_idx, 0));
            let shot_gw = select(tap_ray_count > 0.0, tap_gw, 0.0);
            imposter_quad_index = select(and(imposter_quad_index < 0, shot_gw > 0.0), pi, imposter_quad_index);
            perceptual_rgb_acc += tap_perceptual_geo.rgb * shot_gw;
            geo_weight_acc += shot_gw;
            ray_shortness_acc += tap_perceptual_geo.a * shot_gw;
            center_quad_ray_sum_acc += tap_ray_count * shot_gw;
        }
    }

    let has_geo_weight = geo_weight_acc > 0.0;
    let mean_perceptual_rgb = select(has_geo_weight, perceptual_rgb_acc / geo_weight_acc, f32x3(0.0));
    let ray_shortness_mean = select(has_geo_weight, ray_shortness_acc / geo_weight_acc, 0.0);
    let is_pixel_matching_quad = center_surface_weight_acc > 0.0;
    let total_quad_taps = f32(QUAD_FILTER_DIM * QUAD_FILTER_DIM - 1 + 4);
    let neighborhood_ray_count_geo = ring_ray_count_geo_acc + center_quad_ray_sum_acc;
    let neighborhood_sample_fitness = min(neighborhood_ray_count_geo, total_quad_taps) / total_quad_taps;
    let neighborhood_sample_fitness_sharp = min(neighborhood_sample_fitness, 0.25) * 4.0;

    let ray_count = ray_count_image.Load(i32x3(clamped_index, 0));
    let ray_offset = pixel_ray_alloc_image.Load(i32x3(clamped_index, 0));

    var filtered_diffuse = f32x4(0.0);
    var filtered_diffuse2 = f32x2(0.0);
    var firefly_energy_factor = 1.0;
    {
        // a sparse neighbourhood tightens the geometry aware mean with the plain one so outliers cannot lift the ceiling
        let is_tightening_ceiling = neighborhood_sample_fitness_sharp < 1.0;
        let perceptual_rgb_for_ceiling = firefly_ceiling_reference(
            ff_perceptual_rgb_sum_geo,
            ff_weight_geo,
            ff_perceptual_rgb_sum,
            ff_weight,
            mean_perceptual_rgb,
            is_tightening_ceiling
        );
        let perceptual_tolerance = settings.firefly_perceptual_tolerance * select(is_pixel_matching_quad, 1.0, 0.1);
        let base_ceil_rgb = exp(perceptual_rgb_for_ceiling + perceptual_tolerance);
        let base_ceil_merged = dot(base_ceil_rgb, CHANNEL_PERCEIVED_BRIGHTNESS);

        let is_clamping = settings.firefly_filter_enabled != 0;
        let is_mono = settings.firefly_clamp_mode == 1;
        var energy_pre = 0.0;
        var energy_post = 0.0;

        // the pixel's rays count as ring members, each ray takes itself out again before it is clamped
        let is_using_siblings = and(is_clamping, ray_count > 1u);
        var sibling_sum = f32x3(0.0);
        for (u32 s = 0u; s < select(is_using_siblings, ray_count, 0u); s++) {
            sibling_sum += rtgi_linear_to_perceptual_rgb(ray_result[ray_offset + s].radiance, exposure);
        }

        sibling_sum *= FIREFLY_SIBLING_RAY_WEIGHT;
        let sibling_weight = select(is_using_siblings, f32(ray_count - 1u) * FIREFLY_SIBLING_RAY_WEIGHT, 0.0);

        let inv_count = 1.0 / f32(max(ray_count, 1u));
        for (u32 s = 0u; s < ray_count; s++) {
            let result = ray_result[ray_offset + s];
            let ray_rgb = result.radiance;

            let others_sum = sibling_sum - rtgi_linear_to_perceptual_rgb(ray_rgb, exposure) * FIREFLY_SIBLING_RAY_WEIGHT;
            let sibling_reference = firefly_ceiling_reference(
                ff_perceptual_rgb_sum_geo + others_sum,
                ff_weight_geo + sibling_weight,
                ff_perceptual_rgb_sum + others_sum,
                ff_weight + sibling_weight,
                mean_perceptual_rgb,
                is_tightening_ceiling
            );
            let sibling_ceil_rgb = exp(sibling_reference + perceptual_tolerance);
            let ceil_rgb = select(is_using_siblings, sibling_ceil_rgb, base_ceil_rgb);
            let ceil_merged = select(is_using_siblings, dot(sibling_ceil_rgb, CHANNEL_PERCEIVED_BRIGHTNESS), base_ceil_merged);

            // both modes scale all channels by one ratio, mono merges to a brightness first and rgb takes the worst channel
            let per_channel = (ray_rgb * CHANNEL_PERCEIVED_BRIGHTNESS) / ceil_rgb;
            let over = select(
                is_mono,
                dot(ray_rgb, CHANNEL_PERCEIVED_BRIGHTNESS) / ceil_merged,
                max(per_channel.r, max(per_channel.g, per_channel.b))
            );
            let clamped = select(is_clamping, ray_rgb * firefly_softplus_clamp_factor(over, FIREFLY_SOFTPLUS_SHARPNESS), ray_rgb);

            f32x4 ray_sh_y;
            f32x2 ray_co_cg;
            rtgi_radiance_to_y_co_cg_sh(clamped, com::decompress_normal_32(result.packed_dir), ray_sh_y, ray_co_cg);
            filtered_diffuse += ray_sh_y * inv_count;
            filtered_diffuse2 += ray_co_cg * inv_count;
            energy_pre += dot(ray_rgb, f32x3(1.0)) * inv_count;
            energy_post += dot(clamped, f32x3(1.0)) * inv_count;
        }

        firefly_energy_factor = select(and(is_clamping, energy_post > 1.0e-8), energy_pre / max(energy_post, 1.0e-8), 1.0);
    }

    let ao_guide = 1.0 - sqrt(ray_shortness_mean);
    let radiance_mean_perceptual = select(
        has_geo_weight,
        rtgi_perceptual_radiance_from_rgb(mean_perceptual_rgb),
        rtgi_linear_to_perceptual(0.0, exposure)
    );

    // quad mate imposter fill, a pixel without rays copies the first same surface mate that shot
    gs_own_sh[gtid.x][gtid.y] = filtered_diffuse;
    gs_own_co_cg[gtid.x][gtid.y] = filtered_diffuse2;
    gs_own_factor[gtid.x][gtid.y] = firefly_energy_factor;

    GroupMemoryBarrierWithGroupSync();

    let has_value = and(depth != 0.0, or(ray_count > 0u, imposter_quad_index >= 0));
    let is_imposter = and(imposter_quad_index >= 0, ray_count == 0u);
    // without an imposter the index is -1, clamping it keeps the unused mate read inside the tile
    let mate_quad_index = max(imposter_quad_index, 0);
    let mate = pixel_quad_id * 2 + i32x2(mate_quad_index & 1, mate_quad_index >> 1);
    filtered_diffuse = select(is_imposter, gs_own_sh[mate.x][mate.y], filtered_diffuse);
    filtered_diffuse2 = select(is_imposter, gs_own_co_cg[mate.x][mate.y], filtered_diffuse2);
    firefly_energy_factor = select(is_imposter, gs_own_factor[mate.x][mate.y], firefly_energy_factor);

    gs_filled_sh[gtid.x][gtid.y] = filtered_diffuse;
    gs_filled_co_cg[gtid.x][gtid.y] = filtered_diffuse2;
    gs_filled_factor[gtid.x][gtid.y] = firefly_energy_factor;
    gs_filled_has_value[gtid.x][gtid.y] = f32(has_value);

    GroupMemoryBarrierWithGroupSync();

    // a pixel whose whole quad shot nothing copies the same spot of the quad two pixels to the side
    let pair = i32x2(gtid.x ^ 2, gtid.y);
    let is_pair_valid = gs_filled_has_value[pair.x][pair.y] > 0.5;
    let pair_gs = pair + TOTAL_FILTER_REACH;
    let pair_position = gs_preload_position_depth[pair_gs.x][pair_gs.y].xyz;
    let pair_normal = com::decompress_normal_32(gs_preload_normal[pair_gs.x][pair_gs.y]);
    let is_same_surface = rtgi_calc_similar_surface_weight(
        rcp_pixel_width_ws, world_position, pixel_face_normal, pair_position, pair_normal, 4.0
    ) > 0.5;
    let is_pair_fill = and(and(depth != 0.0, !has_value), and(is_pair_valid, is_same_surface));
    filtered_diffuse = select(is_pair_fill, gs_filled_sh[pair.x][pair.y], filtered_diffuse);
    filtered_diffuse2 = select(is_pair_fill, gs_filled_co_cg[pair.x][pair.y], filtered_diffuse2);
    firefly_energy_factor = select(is_pair_fill, gs_filled_factor[pair.x][pair.y], firefly_energy_factor);

    if (all(dtid < i32x2(settings.half_size))) {
        let pixel = u32x2(dtid);
        pre_filtered_diffuse_image[pixel] = filtered_diffuse;
        pre_filtered_diffuse2_image[pixel] = filtered_diffuse2;
        firefly_factor_image[pixel] = min(firefly_energy_factor, com::HALF_MAX);
        perceptual_radiance_image[pixel] = radiance_mean_perceptual;
        ao_guide_image[pixel] = saturate(ao_guide);
    }
}
