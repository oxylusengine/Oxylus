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

[[vk::binding(0, 0)]] ConstantBuffer<HistogramLuminance> exposure_buffer;
[[vk::binding(1, 0)]] Texture2D<f32> roughness_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> radiance_image;
[[vk::binding(3, 0)]] Texture2D<f32> variance_image;
[[vk::binding(4, 0)]] Texture2D<f32> sample_count_image;
[[vk::binding(5, 0)]] Texture2D<f32x4> reprojected_radiance_image;
[[vk::binding(6, 0)]] Texture2D<f32x4> average_radiance_image;
[[vk::binding(7, 0)]] Sampler linear_sampler;
[[vk::binding(8, 0)]] StructuredBuffer<u32> tile_list;
[[vk::binding(9, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> resolved_radiance_image;
[[vk::binding(10, 0)]] StorageImage2D<f32, ImageFormat::R16F> resolved_variance_image;

groupshared f32x4 g_shared[16][16];

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

func initialize_group_shared_memory(i32x2 dispatch_thread_id, i32x2 group_thread_id, i32x2 screen_size) -> void {
    const i32x2 offset[4] = { i32x2(0, 0), i32x2(8, 0), i32x2(0, 8), i32x2(8, 8) };

    f32x3 radiance[4];

    dispatch_thread_id -= 4;

    for (i32 i = 0; i < 4; ++i) {
        // a load outside the image is undefined, so texels past the render area read as empty
        let pixel_coordinate = dispatch_thread_id + offset[i];
        let is_inside = all(pixel_coordinate >= 0) && all(pixel_coordinate < screen_size);
        radiance[i] = is_inside ? radiance_image.Load(i32x3(pixel_coordinate, 0)).xyz : f32x3(0.0);
    }

    for (i32 j = 0; j < 4; ++j) {
        let cell = group_thread_id + offset[j];
        g_shared[cell.y][cell.x] = f32x4(radiance[j], 0.0);
    }
}

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 group_id : SV_GroupID, u32 group_index : SV_GroupIndex, uniform SSSRSettings settings) -> void {
    let packed_coords = tile_list[group_id.x];
    let tile_origin = i32x2(i32(packed_coords & 0xffffu), i32((packed_coords >> 16) & 0xffffu));
    var group_thread_id = i32x2(sssr_remap_for_wave_reduction(group_index));
    let dispatch_thread_id = tile_origin + group_thread_id;
    let screen_size = i32x2(settings.render_size);
    let is_in_bounds = all(dispatch_thread_id < screen_size);
    let history_clip_weight = settings.temporal_stability;
    let exposure = sssr_current_exposure(settings, exposure_buffer.exposure);

    initialize_group_shared_memory(dispatch_thread_id, group_thread_id, screen_size);
    GroupMemoryBarrierWithGroupSync();

    group_thread_id += 4; // Center threads in groupshared memory

    let radiance = g_shared[group_thread_id.y][group_thread_id.x].xyz;
    let radiance_sum = radiance.x + radiance.y + radiance.z;
    if (radiance_sum < SSSR_DNSR_RADIANCE_THRESHOLD) {
        if (is_in_bounds) {
            resolved_radiance_image[dispatch_thread_id] = f32x4(0.0);
            resolved_variance_image[dispatch_thread_id] = 0.0;
        }

        return;
    }

    var new_signal = radiance;
    let roughness = is_in_bounds ? sssr_roughness_to_alpha(roughness_image.Load(i32x3(dispatch_thread_id, 0))) : 1.0;
    var new_variance = is_in_bounds ? variance_image.Load(i32x3(dispatch_thread_id, 0)) : 0.0;

    if (is_in_bounds && sssr_is_glossy_reflection(roughness, settings.roughness_threshold)) {
        let num_samples = sample_count_image.Load(i32x3(dispatch_thread_id, 0));
        let uv8 = (f32x2(dispatch_thread_id) + 0.5) / f32x2((settings.render_size + 7u) / 8u * 8u);
        let avg_radiance = average_radiance_image.SampleLevel(linear_sampler, uv8, 0.0).xyz;

        let old_signal = reprojected_radiance_image.Load(i32x3(dispatch_thread_id, 0)).xyz;
        var local_neighborhood = estimate_local_neighborhood_in_group(group_thread_id);
        let color_std = (sqrt(local_neighborhood.variance.xyz) + length(local_neighborhood.mean.xyz - avg_radiance)) * history_clip_weight * 1.4;
        local_neighborhood.mean.xyz = lerp(local_neighborhood.mean.xyz, avg_radiance, f32x3(0.2, 0.2, 0.2));
        let radiance_min = local_neighborhood.mean.xyz - color_std;
        let radiance_max = local_neighborhood.mean.xyz + color_std;
        // Clip history samples
        let clipped_old_signal = sssr_dnsr_clip_aabb(radiance_min, radiance_max, old_signal.xyz);
        // Blend with average for small sample count
        let accumulation_speed = 1.0 / max(num_samples, 1.0);
        let weight = (1.0 - accumulation_speed);
        // Clip outliers from the new sample
        new_signal.xyz = lerp(new_signal.xyz, avg_radiance, 1.0 / max(num_samples + 1.0, 1.0));
        {
            let clip_min = avg_radiance.xyz - color_std * f32x3(1.0, 1.0, 1.0);
            let clip_max = avg_radiance.xyz + color_std * f32x3(1.0, 1.0, 1.0);
            new_signal.xyz = sssr_dnsr_clip_aabb(clip_min, clip_max, new_signal.xyz);
        }

        new_signal = lerp(new_signal, clipped_old_signal, weight);
        new_variance = lerp(sssr_dnsr_compute_temporal_variance(new_signal.xyz, clipped_old_signal.xyz), new_variance, weight);
        if (any(isinf(new_signal)) || any(isnan(new_signal)) || isinf(new_variance) || isnan(new_variance)) {
            new_signal = f32x3(0.0, 0.0, 0.0);
            new_variance = 0.0;
        }
    }

    if (is_in_bounds) {
        // history stays raw, so the exposure used for the denoiser constants comes back out here
        resolved_radiance_image[dispatch_thread_id] = f32x4(new_signal / exposure, 0.0);
        resolved_variance_image[dispatch_thread_id] = new_variance;
    }
}
