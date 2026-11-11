import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(2, 0)]] Texture2D<u32> half_res_face_normal_image;
[[vk::binding(3, 0)]] Texture2D<f32> previous_half_res_depth_image;
[[vk::binding(4, 0)]] Texture2D<u32> previous_half_res_face_normal_image;
[[vk::binding(5, 0)]] [[format("r16ui")]] Texture2D<u32> previous_sample_count_image;
[[vk::binding(6, 0)]] [[format("r16ui")]] RWTexture2D<u32> sample_count_image;
[[vk::binding(7, 0)]] [[format("rg16ui")]] RWTexture2D<u32x2> reproject_corner_image;
[[vk::binding(8, 0)]] RWTexture2D<f32x4> reproject_weights_image;
[[vk::binding(9, 0)]] RWTexture2D<f32x4> ray_impact_image;
[[vk::binding(10, 0)]] RWStructuredBuffer<RTGIRayCounters> counters;

groupshared u32 gs_tile_total_desired;
groupshared u32 gs_tile_geo_count;

// returns the rays this pixel wants, 0 for sky and out of bounds pixels
func reproject_pixel(u32x2 half_pixel, in RTGISettings settings) -> u32 {
    if (any(half_pixel >= settings.half_size)) {
        return 0u;
    }

    let load = u32x3(half_pixel, 0u);
    let depth = half_res_depth_image.Load(load);
    if (depth == 0.0) {
        sample_count_image[half_pixel] = rtgi_pack_sample_counts_sky();
        reproject_corner_image[half_pixel] = u32x2(0u);
        reproject_weights_image[half_pixel] = f32x4(0.0);
        ray_impact_image[half_pixel] = f32x4(rtgi_pack_ray_impact_factor(1.0), 0.0, 0.0, 0.0);
        return 0u;
    }

    let half_size = f32x2(settings.half_size);
    let max_texel = i32x2(settings.half_size) - 1;
    let uv = (f32x2(half_pixel) + 0.5) / half_size;
    let position_ws = camera.world_position_from_linear_depth(uv, depth);
    let normal_ws = com::decompress_normal_32(half_res_face_normal_image.Load(load));
    let pixel_width_ws = rtgi_pixel_width_ws(camera, uv, depth);
    let rcp_pixel_width_ws = 1.0 / pixel_width_ws;

    // the history was rasterized with the previous frame's jitter
    let previous_clip = mul(camera.previous_projection_view, f32x4(position_ws, 1.0));
    let uv_prev = previous_clip.xy / previous_clip.w * 0.5 + 0.5 + camera.temporalaa_jitter_prev / camera.resolution;
    let is_in_screen = and(previous_clip.w > 0.0, and(all(uv_prev > 0.0), all(uv_prev < 1.0)));
    let in_screen = f32(is_in_screen);

    let bilinear = com::bilinear_filter(saturate(uv_prev), half_size);
    let origin = i32x2(bilinear.origin);
    let previous_camera_position = camera.previous_position();
    let expected_distance = distance(previous_camera_position, position_ws);

    var counts = f32x4(0.0);
    var normal_similarity = f32x4(1.0);
    var surface_weights = f32x4(0.0);
    var occluder_taps = f32x4(0.0);
    for (u32 t = 0u; t < 4u; t++) {
        let offset = i32x2(i32(t & 1u), i32(t >> 1u));
        let tap_load = u32x3(u32x2(clamp(origin + offset, i32x2(0), max_texel)), 0u);
        let tap_depth = previous_half_res_depth_image.Load(tap_load);
        let tap_normal = com::decompress_normal_32(previous_half_res_face_normal_image.Load(tap_load));
        counts[t] = rtgi_unpack_normal_count(previous_sample_count_image.Load(tap_load));
        normal_similarity[t] = rtgi_calc_similar_normal_weight(tap_normal, normal_ws);

        let tap_uv = (f32x2(origin + offset) + 0.5) / half_size;
        let tap_position = camera.previous_world_position_from_linear_depth(tap_uv, tap_depth);
        let surface_weight = rtgi_calc_similar_surface_weight_dist_limited(
            rcp_pixel_width_ws, position_ws, normal_ws, tap_position, tap_normal, 2.0
        );
        surface_weights[t] = select(tap_depth != 0.0, surface_weight, 0.0);

        // a failed tap in front of the expected point occluded it last frame, one behind it is only an overhanging edge
        let is_failed = and(surface_weights[t] <= 0.0, tap_depth != 0.0);
        let is_nearer = distance(previous_camera_position, tap_position) < expected_distance - 2.0 * pixel_width_ws;
        occluder_taps[t] = f32(and(is_failed, is_nearer));
    }

    let sample_weights = com::bilinear_weights(bilinear) * (surface_weights * in_screen * normal_similarity);
    let is_disocclusion = dot(sample_weights, f32x4(1.0)) < 0.025;

    let occluder_coverage = dot(com::bilinear_weights(bilinear), occluder_taps);
    let reprojected_count = rtgi_reproject_count_occluder_aware(counts, sample_weights, occluder_coverage);
    let carried_count = select(
        or(is_disocclusion, isnan(reprojected_count)),
        0.0,
        min(RTGI_COUNT_NORMAL_MAX, reprojected_count)
    );

    // a strength of 0 gives a penalty of 0
    let parallax_penalty = rtgi_calc_parallax_penalty(
        position_ws,
        normal_ws,
        camera.position.xyz,
        previous_camera_position,
        max(settings.temporal_parallax_penalty_strength, 0.0)
    );

    // an active penalty first caps the history at the ray demand target, then scales it
    let history_count = select(
        parallax_penalty > 0.0,
        min(carried_count, settings.fast_convergence_samples) * (1.0 - parallax_penalty),
        carried_count
    );

    // the fast field borrows the parallax penalty for the accumulate pass, which overwrites it with the real fast count
    sample_count_image[half_pixel] = rtgi_pack_sample_counts(history_count, parallax_penalty * 15.0);
    reproject_corner_image[half_pixel] = u32x2(origin + 1);
    reproject_weights_image[half_pixel] = sample_weights;

    // without specular the share and material factors are 1
    let impact_diffuse = 1.0;
    ray_impact_image[half_pixel] = f32x4(rtgi_pack_ray_impact_factor(impact_diffuse), 0.0, 0.0, 0.0);
    return rtgi_calc_ray_demand(history_count, impact_diffuse, settings);
}

[[shader("compute")]]
[numthreads(16, 16, 1)]
func cs_main(
    u32x2 half_pixel : SV_DispatchThreadID,
    u32x2 group_thread : SV_GroupThreadID,
    uniform RTGISettings settings
) {
    if (all(group_thread == 0u)) {
        gs_tile_total_desired = 0u;
        gs_tile_geo_count = 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    let demand = reproject_pixel(half_pixel, settings);
    InterlockedAdd(gs_tile_total_desired, demand);
    InterlockedAdd(gs_tile_geo_count, u32(demand > 0u));

    GroupMemoryBarrierWithGroupSync();

    if (all(group_thread == 0u)) {
        let tile_extra = gs_tile_total_desired - gs_tile_geo_count;
        if (tile_extra > 0u) {
            InterlockedAdd(counters[0].total_extra_rays, tile_extra);
        }

        if (gs_tile_geo_count > 0u) {
            InterlockedAdd(counters[0].total_geo_rays, gs_tile_geo_count);
        }
    }
}
