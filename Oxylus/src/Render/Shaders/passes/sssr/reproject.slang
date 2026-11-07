// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

import common;
import gpu;
import scene;
import passes.sssr.sssr_lib;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] ConstantBuffer<HistogramLuminance> exposure_buffer;
[[vk::binding(2, 0)]] Texture2D<f32> depth_image;
[[vk::binding(3, 0)]] Texture2D<f32> previous_depth_image;
[[vk::binding(4, 0)]] Texture2D<f32> roughness_image;
[[vk::binding(5, 0)]] Texture2D<f32> previous_roughness_image;
[[vk::binding(6, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(7, 0)]] Texture2D<f32x4> previous_normal_image;
[[vk::binding(8, 0)]] Texture2D<f32x4> radiance_image;
[[vk::binding(9, 0)]] Texture2D<f32x4> previous_radiance_image;
[[vk::binding(10, 0)]] Texture2D<f32x2> velocity_image;
[[vk::binding(11, 0)]] Texture2D<f32> previous_variance_image;
[[vk::binding(12, 0)]] Texture2D<f32> previous_sample_count_image;
[[vk::binding(13, 0)]] Sampler linear_sampler;
[[vk::binding(14, 0)]] StructuredBuffer<u32> tile_list;
[[vk::binding(15, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> reprojected_radiance_image;
[[vk::binding(16, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> average_radiance_image;
[[vk::binding(17, 0)]] StorageImage2D<f32, ImageFormat::R16F> variance_image;
[[vk::binding(18, 0)]] StorageImage2D<f32, ImageFormat::R16F> sample_count_image;

groupshared f32x4 g_shared[16][16];

static f32 g_exposure = 1.0;
static u32x2 g_render_size = u32x2(0);

func is_in_bounds(i32x2 texel) -> bool {
    return all(texel >= 0) && all(texel < i32x2(g_render_size));
}

func load_radiance(i32x2 texel) -> f32x3 {
    return is_in_bounds(texel) ? radiance_image.Load(i32x3(texel, 0)).xyz : f32x3(0.0);
}

func load_ray_length(i32x2 texel) -> f32 {
    return is_in_bounds(texel) ? radiance_image.Load(i32x3(texel, 0)).w : 0.0;
}

func load_roughness(i32x2 texel) -> f32 {
    return is_in_bounds(texel) ? sssr_roughness_to_alpha(roughness_image.Load(i32x3(texel, 0))) : 1.0;
}

func load_world_space_normal(i32x2 texel) -> f32x3 {
    return is_in_bounds(texel) ? sssr_safe_normalize(normal_image.Load(i32x3(texel, 0)).xyz) : f32x3(0.0);
}

func load_depth(i32x2 texel) -> f32 {
    return is_in_bounds(texel) ? depth_image.Load(i32x3(texel, 0)) : 0.0;
}

func load_radiance_history(i32x2 texel) -> f32x3 {
    return is_in_bounds(texel) ? g_exposure * previous_radiance_image.Load(i32x3(texel, 0)).xyz : f32x3(0.0);
}

func load_world_space_normal_history(i32x2 texel) -> f32x3 {
    return is_in_bounds(texel) ? sssr_safe_normalize(previous_normal_image.Load(i32x3(texel, 0)).xyz) : f32x3(0.0);
}

func load_depth_history(i32x2 texel) -> f32 {
    return is_in_bounds(texel) ? previous_depth_image.Load(i32x3(texel, 0)) : 0.0;
}

func sample_radiance_history(f32x2 uv) -> f32x3 {
    return g_exposure * previous_radiance_image.SampleLevel(linear_sampler, uv, 0.0).xyz;
}

func sample_world_space_normal_history(f32x2 uv) -> f32x3 {
    return sssr_safe_normalize(previous_normal_image.SampleLevel(linear_sampler, uv, 0.0).xyz);
}

func sample_roughness_history(f32x2 uv) -> f32 {
    return sssr_roughness_to_alpha(previous_roughness_image.SampleLevel(linear_sampler, uv, 0.0));
}

func sample_variance_history(f32x2 uv) -> f32 {
    return previous_variance_image.SampleLevel(linear_sampler, uv, 0.0);
}

func sample_num_samples_history(f32x2 uv) -> f32 {
    return previous_sample_count_image.SampleLevel(linear_sampler, uv, 0.0);
}

// the depth history is padded to a multiple of 64 and zero outside the render area, so the uv is clamped to the
// last render texel and rescaled to keep the edge from blending with the padding
func sample_depth_history(f32x2 uv) -> f32 {
    let render = f32x2(g_render_size);
    let padded = f32x2((g_render_size + 63u) / 64u * 64u);
    let clamped = clamp(uv, 0.5 / render, 1.0 - 0.5 / render);
    return previous_depth_image.SampleLevel(linear_sampler, clamped * render / padded, 0.0);
}

func initialize_group_shared_memory(i32x2 dispatch_thread_id, i32x2 group_thread_id) -> void {
    const i32x2 offset[4] = { i32x2(0, 0), i32x2(8, 0), i32x2(0, 8), i32x2(8, 8) };

    f32x3 radiance[4];

    dispatch_thread_id -= 4;

    for (i32 i = 0; i < 4; ++i) {
        radiance[i] = load_radiance(dispatch_thread_id + offset[i]);
    }

    for (i32 j = 0; j < 4; ++j) {
        let cell = group_thread_id + offset[j];
        g_shared[cell.y][cell.x] = f32x4(radiance[j], 0.0);
    }
}

func get_luminance_weight(f32x3 val) -> f32 {
    let luma = sssr_dnsr_luminance(val);
    return max(exp(-luma * SSSR_DNSR_AVG_RADIANCE_LUMINANCE_WEIGHT), 1.0e-2);
}

// velocity is the previous position minus this one in render pixels without the jitter, history is rasterized with
// the previous frame's jitter
func get_surface_reprojection(f32x2 uv, f32x2 velocity) -> f32x2 {
    return uv + velocity / camera.resolution - camera.temporalaa_jitter / camera.resolution + camera.temporalaa_jitter_prev / camera.resolution;
}

func get_hit_position_reprojection(i32x2 dispatch_thread_id, f32x2 uv, f32 reflected_ray_length) -> f32x2 {
    let z = load_depth(dispatch_thread_id);
    var view_space_ray = sssr_screen_to_view(camera, uv, z);

    let surface_depth = length(view_space_ray);
    let ray_length = surface_depth + reflected_ray_length;

    view_space_ray /= surface_depth; // == normalize(view_space_ray)
    view_space_ray *= ray_length;
    // This is the "fake" hit position if we would follow the ray straight through the surface.
    let world_hit_position = mul(camera.inv_view, f32x4(view_space_ray, 1.0)).xyz;

    // behind the previous camera there is no history, the uv lands outside the screen and gets rejected
    let previous_clip = mul(camera.previous_projection_view, f32x4(world_hit_position, 1.0));
    if (previous_clip.w <= 0.0) {
        return f32x2(-1.0);
    }

    return previous_clip.xy / previous_clip.w * 0.5 + 0.5 + camera.temporalaa_jitter_prev / camera.resolution;
}

func get_disocclusion_factor(f32x3 normal, f32x3 history_normal, f32 linear_depth, f32 history_linear_depth) -> f32 {
    return 1.0 //
        * exp(-abs(1.0 - max(0.0, dot(normal, history_normal))) * SSSR_DNSR_DISOCCLUSION_NORMAL_WEIGHT) //
        * exp(-abs(history_linear_depth - linear_depth) / linear_depth * SSSR_DNSR_DISOCCLUSION_DEPTH_WEIGHT);
}

struct Moments {
    f32x3 mean;
    f32x3 variance;
};

func estimate_local_neighborhood_in_group(i32x2 group_thread_id) -> Moments {
    Moments estimate;
    estimate.mean = f32x3(0.0);
    estimate.variance = f32x3(0.0);
    f32 accumulated_weight = 0.0;
    for (i32 j = -SSSR_DNSR_LOCAL_NEIGHBORHOOD_RADIUS; j <= SSSR_DNSR_LOCAL_NEIGHBORHOOD_RADIUS; ++j) {
        for (i32 i = -SSSR_DNSR_LOCAL_NEIGHBORHOOD_RADIUS; i <= SSSR_DNSR_LOCAL_NEIGHBORHOOD_RADIUS; ++i) {
            let new_idx = group_thread_id + i32x2(i, j);
            let radiance = g_shared[new_idx.y][new_idx.x].xyz;
            let weight = sssr_dnsr_local_neighborhood_kernel_weight(f32(i)) * sssr_dnsr_local_neighborhood_kernel_weight(f32(j));
            accumulated_weight += weight;
            estimate.mean += radiance * weight;
            estimate.variance += radiance * radiance * weight;
        }
    }

    estimate.mean /= accumulated_weight;
    estimate.variance /= accumulated_weight;

    estimate.variance = abs(estimate.variance - estimate.mean * estimate.mean);
    return estimate;
}

func dot2(f32x3 a) -> f32 {
    return dot(a, a);
}

func pick_reprojection(
    i32x2 dispatch_thread_id,
    i32x2 group_thread_id,
    u32x2 screen_size,
    f32 roughness,
    f32 ray_length,
    out f32 disocclusion_factor,
    out f32x2 reprojection_uv,
    out f32x3 reprojection
) -> void {
    disocclusion_factor = 0.0;
    reprojection_uv = f32x2(0.0);
    reprojection = f32x3(0.0);

    let local_neighborhood = estimate_local_neighborhood_in_group(group_thread_id);

    let uv = (f32x2(dispatch_thread_id) + 0.5) / f32x2(screen_size);
    let normal = load_world_space_normal(dispatch_thread_id);
    f32x3 history_normal;
    f32 history_linear_depth;

    {
        let motion_vector = velocity_image.Load(i32x3(dispatch_thread_id, 0)).xy;
        let surface_reprojection_uv = get_surface_reprojection(uv, motion_vector);
        let hit_reprojection_uv = get_hit_position_reprojection(dispatch_thread_id, uv, ray_length);
        let surface_normal = sample_world_space_normal_history(surface_reprojection_uv);
        let hit_normal = sample_world_space_normal_history(hit_reprojection_uv);
        let surface_history = sample_radiance_history(surface_reprojection_uv);
        let hit_history = sample_radiance_history(hit_reprojection_uv);
        let hit_normal_similarity = dot(sssr_safe_normalize(hit_normal), sssr_safe_normalize(normal));
        let surface_normal_similarity = dot(sssr_safe_normalize(surface_normal), sssr_safe_normalize(normal));
        let hit_roughness = sample_roughness_history(hit_reprojection_uv);
        let surface_roughness = sample_roughness_history(surface_reprojection_uv);

        // Candidate for mirror reflection parallax
        if (hit_normal_similarity > SSSR_DNSR_REPROJECTION_NORMAL_SIMILARITY_THRESHOLD //
            && hit_normal_similarity + 1.0e-3 > surface_normal_similarity //
            && abs(hit_roughness - roughness) < abs(surface_roughness - roughness) + 1.0e-3) {
            history_normal = hit_normal;
            let hit_history_depth = sample_depth_history(hit_reprojection_uv);
            let hit_history_linear_depth = sssr_dnsr_linear_depth(camera, hit_reprojection_uv, hit_history_depth);
            history_linear_depth = hit_history_linear_depth;
            reprojection_uv = hit_reprojection_uv;
            reprojection = hit_history;
        } else {
            // Reject surface reprojection based on simple distance
            if (dot2(surface_history - local_neighborhood.mean) < SSSR_DNSR_REPROJECT_SURFACE_DISCARD_VARIANCE_WEIGHT * length(local_neighborhood.variance)) {
                history_normal = surface_normal;
                let surface_history_depth = sample_depth_history(surface_reprojection_uv);
                let surface_history_linear_depth = sssr_dnsr_linear_depth(camera, surface_reprojection_uv, surface_history_depth);
                history_linear_depth = surface_history_linear_depth;
                reprojection_uv = surface_reprojection_uv;
                reprojection = surface_history;
            } else {
                disocclusion_factor = 0.0;
                return;
            }
        }
    }

    let depth = load_depth(dispatch_thread_id);
    let linear_depth = sssr_dnsr_linear_depth(camera, uv, depth);

    // Determine disocclusion factor based on history signal
    disocclusion_factor = get_disocclusion_factor(normal, history_normal, linear_depth, history_linear_depth);

    if (disocclusion_factor > SSSR_DNSR_DISOCCLUSION_THRESHOLD) { // Early out, good enough
        return;
    }

    // Try to find the closest sample in the vicinity if we are not convinced of a disocclusion
    if (disocclusion_factor < SSSR_DNSR_DISOCCLUSION_THRESHOLD) {
        f32x2 dudv = 1.0 / f32x2(screen_size);
        const i32 search_radius = 1;
        for (i32 y = -search_radius; y <= search_radius; y++) {
            for (i32 x = -search_radius; x <= search_radius; x++) {
                let search_uv = reprojection_uv + f32x2(x, y) * dudv;
                let search_normal = sample_world_space_normal_history(search_uv);
                let search_depth = sample_depth_history(search_uv);
                let search_linear_depth = sssr_dnsr_linear_depth(camera, search_uv, search_depth);
                let weight = get_disocclusion_factor(normal, search_normal, linear_depth, search_linear_depth);
                if (weight > disocclusion_factor) {
                    disocclusion_factor = weight;
                    reprojection_uv = search_uv;
                }
            }
        }

        reprojection = sample_radiance_history(reprojection_uv);
    }

    // Try to find the closest sample in the vicinity if we are not convinced of a disocclusion
    if (disocclusion_factor < SSSR_DNSR_DISOCCLUSION_THRESHOLD) {
        let uvx = frac(f32(screen_size.x) * reprojection_uv.x + 0.5);
        let uvy = frac(f32(screen_size.y) * reprojection_uv.y + 0.5);
        let reproject_texel_coords = i32x2(f32x2(screen_size) * reprojection_uv - 0.5);
        let reprojection00 = load_radiance_history(reproject_texel_coords + i32x2(0, 0));
        let reprojection10 = load_radiance_history(reproject_texel_coords + i32x2(1, 0));
        let reprojection01 = load_radiance_history(reproject_texel_coords + i32x2(0, 1));
        let reprojection11 = load_radiance_history(reproject_texel_coords + i32x2(1, 1));
        let normal00 = load_world_space_normal_history(reproject_texel_coords + i32x2(0, 0));
        let normal10 = load_world_space_normal_history(reproject_texel_coords + i32x2(1, 0));
        let normal01 = load_world_space_normal_history(reproject_texel_coords + i32x2(0, 1));
        let normal11 = load_world_space_normal_history(reproject_texel_coords + i32x2(1, 1));
        let depth00 = sssr_dnsr_linear_depth(camera, reprojection_uv, load_depth_history(reproject_texel_coords + i32x2(0, 0)));
        let depth10 = sssr_dnsr_linear_depth(camera, reprojection_uv, load_depth_history(reproject_texel_coords + i32x2(1, 0)));
        let depth01 = sssr_dnsr_linear_depth(camera, reprojection_uv, load_depth_history(reproject_texel_coords + i32x2(0, 1)));
        let depth11 = sssr_dnsr_linear_depth(camera, reprojection_uv, load_depth_history(reproject_texel_coords + i32x2(1, 1)));
        var w = f32x4(1.0, 1.0, 1.0, 1.0);
        // Initialize bilinear weights
        w.x = get_disocclusion_factor(normal, normal00, linear_depth, depth00) > SSSR_DNSR_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w.y = get_disocclusion_factor(normal, normal10, linear_depth, depth10) > SSSR_DNSR_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w.z = get_disocclusion_factor(normal, normal01, linear_depth, depth01) > SSSR_DNSR_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w.w = get_disocclusion_factor(normal, normal11, linear_depth, depth11) > SSSR_DNSR_DISOCCLUSION_THRESHOLD / 2.0 ? 1.0 : 0.0;
        w.x = w.x * (1.0 - uvx) * (1.0 - uvy);
        w.y = w.y * (uvx) * (1.0 - uvy);
        w.z = w.z * (1.0 - uvx) * (uvy);
        w.w = w.w * (uvx) * (uvy);
        // Normalize
        let ws = max(w.x + w.y + w.z + w.w, 1.0e-3);
        w /= ws;

        // Compute the weighted history signal
        reprojection = reprojection00 * w.x + reprojection10 * w.y + reprojection01 * w.z + reprojection11 * w.w;
        let blended_linear_depth = depth00 * w.x + depth10 * w.y + depth01 * w.z + depth11 * w.w;
        let blended_normal = normal00 * w.x + normal10 * w.y + normal01 * w.z + normal11 * w.w;
        disocclusion_factor = get_disocclusion_factor(normal, blended_normal, linear_depth, blended_linear_depth);
    }

    disocclusion_factor = disocclusion_factor < SSSR_DNSR_DISOCCLUSION_THRESHOLD ? 0.0 : disocclusion_factor;
}

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 group_id : SV_GroupID, u32 group_index : SV_GroupIndex, uniform SSSRSettings settings) -> void {
    g_render_size = settings.render_size;
    g_exposure = sssr_current_exposure(settings, exposure_buffer.exposure);

    let packed_coords = tile_list[group_id.x];
    let tile_origin = i32x2(i32(packed_coords & 0xffffu), i32((packed_coords >> 16) & 0xffffu));
    var group_thread_id = i32x2(sssr_remap_for_wave_reduction(group_index));
    let dispatch_thread_id = tile_origin + group_thread_id;
    let screen_size = settings.render_size;
    let is_pixel_in_bounds = is_in_bounds(dispatch_thread_id);

    initialize_group_shared_memory(dispatch_thread_id, group_thread_id);
    GroupMemoryBarrierWithGroupSync();

    group_thread_id += 4; // Center threads in groupshared memory

    f32 roughness = load_roughness(dispatch_thread_id);
    var radiance = load_radiance(dispatch_thread_id);
    let ray_length = load_ray_length(dispatch_thread_id);

    if (is_pixel_in_bounds && sssr_is_glossy_reflection(roughness, settings.roughness_threshold)) {
        f32 disocclusion_factor;
        f32x2 reprojection_uv;
        f32x3 reprojection;
        pick_reprojection(
            dispatch_thread_id,
            group_thread_id,
            screen_size,
            roughness,
            ray_length,
            disocclusion_factor,
            reprojection_uv,
            reprojection
        );

        if ((reprojection_uv.x > 0.0) && (reprojection_uv.y > 0.0) && (reprojection_uv.x < 1.0) && (reprojection_uv.y < 1.0)) {
            let prev_variance = sample_variance_history(reprojection_uv);
            var num_samples = sample_num_samples_history(reprojection_uv) * disocclusion_factor;
            let s_max_samples = max(8.0, f32(SSSR_DNSR_MAX_SAMPLES) * sssr_dnsr_samples_for_roughness(roughness));
            num_samples = min(s_max_samples, num_samples + 1.0);
            let new_variance = sssr_dnsr_compute_temporal_variance(radiance, reprojection);
            if (disocclusion_factor < SSSR_DNSR_DISOCCLUSION_THRESHOLD) {
                reprojected_radiance_image[dispatch_thread_id] = f32x4(0.0);
                variance_image[dispatch_thread_id] = 1.0;
                sample_count_image[dispatch_thread_id] = 1.0;
            } else {
                let variance_mix = lerp(new_variance, prev_variance, 1.0 / num_samples);
                reprojected_radiance_image[dispatch_thread_id] = f32x4(reprojection, 0.0);
                variance_image[dispatch_thread_id] = variance_mix;
                sample_count_image[dispatch_thread_id] = num_samples;
                // Mix in reprojection for radiance mip computation
                radiance = lerp(radiance, reprojection, 0.3);
            }
        } else {
            reprojected_radiance_image[dispatch_thread_id] = f32x4(0.0);
            variance_image[dispatch_thread_id] = 1.0;
            sample_count_image[dispatch_thread_id] = 1.0;
        }
    }

    // Downsample 8x8 -> 1x1
    // Weight by luminance to prevent bright pixels from dominating the average
    var weight = get_luminance_weight(radiance);
    radiance *= weight;
    if (!is_pixel_in_bounds || any(isinf(radiance)) || any(isnan(radiance)) || (weight > 1.0e3)) {
        radiance = f32x3(0.0);
        weight = 0.0;
    }

    group_thread_id -= 4; // Center threads in groupshared memory

    // the neighborhood reads above must finish before the low cells are reused for the downsample
    GroupMemoryBarrierWithGroupSync();

    g_shared[group_thread_id.y][group_thread_id.x] = f32x4(radiance, weight);
    GroupMemoryBarrierWithGroupSync();

    for (i32 i = 2; i <= 8; i = i * 2) {
        let ox = group_thread_id.x * i;
        let oy = group_thread_id.y * i;
        let ix = group_thread_id.x * i + i / 2;
        let iy = group_thread_id.y * i + i / 2;
        if (ix < 8 && iy < 8) {
            let rad_weight00 = g_shared[oy][ox];
            let rad_weight10 = g_shared[iy][ox];
            let rad_weight01 = g_shared[oy][ix];
            let rad_weight11 = g_shared[iy][ix];
            let sum = rad_weight00 + rad_weight01 + rad_weight10 + rad_weight11;
            g_shared[oy][ox] = sum;
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if ((group_thread_id.x == 0) && (group_thread_id.y == 0)) {
        let sum = g_shared[0][0];
        let weight_acc = max(sum.w, 1.0e-3);
        let radiance_avg = sum.xyz / weight_acc;
        average_radiance_image[dispatch_thread_id.xy / 8] = f32x4(radiance_avg, 0.0);
    }
}
