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
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] Texture2D<f32> roughness_image;
[[vk::binding(3, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(4, 0)]] Texture2D<f32x4> radiance_image;
[[vk::binding(5, 0)]] Texture2D<f32> variance_image;
[[vk::binding(6, 0)]] Texture2D<f32x4> average_radiance_image;
[[vk::binding(7, 0)]] Sampler linear_sampler;
[[vk::binding(8, 0)]] StructuredBuffer<u32> tile_list;
[[vk::binding(9, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> prefiltered_radiance_image;
[[vk::binding(10, 0)]] StorageImage2D<f32, ImageFormat::R16F> prefiltered_variance_image;

groupshared f32x4 g_radiance_variance[16][16];
groupshared f32x4 g_normal_depth[16][16];

struct NeighborhoodSample {
    f32x3 radiance;
    f32 variance;
    f32x3 normal;
    f32 depth;
};

func load_from_group_shared_memory(i32x2 idx) -> NeighborhoodSample {
    let radiance_variance = g_radiance_variance[idx.y][idx.x];
    let normal_depth = g_normal_depth[idx.y][idx.x];

    NeighborhoodSample neighbor_sample;
    neighbor_sample.radiance = radiance_variance.xyz;
    neighbor_sample.variance = radiance_variance.w;
    neighbor_sample.normal = normal_depth.xyz;
    neighbor_sample.depth = normal_depth.w;
    return neighbor_sample;
}

func store_in_group_shared_memory(i32x2 group_thread_id, f32x3 radiance, f32 variance, f32x3 normal, f32 depth) -> void {
    g_radiance_variance[group_thread_id.y][group_thread_id.x] = f32x4(radiance, variance);
    g_normal_depth[group_thread_id.y][group_thread_id.x] = f32x4(normal, depth);
}

// a load outside the image is undefined, so texels past the render area read as empty
func load_neighborhood(
    i32x2 pixel_coordinate,
    out f32x3 radiance,
    out f32 variance,
    out f32x3 normal,
    out f32 depth,
    i32x2 screen_size
) -> void {
    if (any(pixel_coordinate < 0) || any(pixel_coordinate >= screen_size)) {
        radiance = f32x3(0.0);
        variance = 0.0;
        normal = f32x3(0.0);
        depth = sssr_dnsr_linear_depth(camera, f32x2(0.0), 0.0);
        return;
    }

    radiance = radiance_image.Load(i32x3(pixel_coordinate, 0)).xyz;
    variance = variance_image.Load(i32x3(pixel_coordinate, 0));

    normal = sssr_safe_normalize(normal_image.Load(i32x3(pixel_coordinate, 0)).xyz);

    let uv = (f32x2(pixel_coordinate) + 0.5) / f32x2(screen_size);
    depth = sssr_dnsr_linear_depth(camera, uv, depth_image.Load(i32x3(pixel_coordinate, 0)));
}

func initialize_group_shared_memory(i32x2 dispatch_thread_id, i32x2 group_thread_id, i32x2 screen_size) -> void {
    // Load 16x16 region into shared memory using 4 8x8 blocks.
    const i32x2 offset[4] = { i32x2(0, 0), i32x2(8, 0), i32x2(0, 8), i32x2(8, 8) };

    // Intermediate storage registers to cache the result of all loads
    f32x3 radiance[4];
    f32 variance[4];
    f32x3 normal[4];
    f32 depth[4];

    dispatch_thread_id -= 4;

    for (i32 i = 0; i < 4; ++i) {
        load_neighborhood(dispatch_thread_id + offset[i], radiance[i], variance[i], normal[i], depth[i], screen_size);
    }

    for (i32 j = 0; j < 4; ++j) {
        store_in_group_shared_memory(group_thread_id + offset[j], radiance[j], variance[j], normal[j], depth[j]);
    }
}

func get_edge_stopping_normal_weight(f32x3 normal_p, f32x3 normal_q) -> f32 {
    return pow(max(dot(normal_p, normal_q), 0.0), SSSR_DNSR_PREFILTER_NORMAL_SIGMA);
}

func get_edge_stopping_depth_weight(f32 center_depth, f32 neighbor_depth) -> f32 {
    return exp(-abs(center_depth - neighbor_depth) * center_depth * SSSR_DNSR_PREFILTER_DEPTH_SIGMA);
}

func get_radiance_weight(f32x3 center_radiance, f32x3 neighbor_radiance, f32 variance) -> f32 {
    return max(exp(-(SSSR_DNSR_RADIANCE_WEIGHT_BIAS + variance * SSSR_DNSR_RADIANCE_WEIGHT_VARIANCE_K) * length(center_radiance - neighbor_radiance.xyz)), 1.0e-2);
}

func resolve_neighborhood(i32x2 group_thread_id, f32x3 avg_radiance, NeighborhoodSample center, out f32x3 resolved_radiance, out f32 resolved_variance) -> void {
    // Initial weight is important to remove fireflies.
    // That removes quite a bit of energy but makes everything much more stable.
    var accumulated_weight = get_radiance_weight(avg_radiance, center.radiance.xyz, center.variance);
    var accumulated_radiance = center.radiance.xyz * accumulated_weight;
    var accumulated_variance = center.variance * accumulated_weight * accumulated_weight;
    // First 15 samples from Halton(2,3) streteched along the Z axis
    const u32 sample_count = 15;
    const i32x2 sample_offsets[15] = {
        i32x2(0, 1), i32x2(-2, 1), i32x2(2, -3), i32x2(-3, 0), i32x2(1, 2), i32x2(-1, -2), i32x2(3, 0), i32x2(-3, 3),
        i32x2(0, -3), i32x2(-1, -1), i32x2(2, 1), i32x2(-2, -2), i32x2(1, 0), i32x2(0, 2), i32x2(3, -1)
    };
    let variance_weight = max(SSSR_DNSR_PREFILTER_VARIANCE_BIAS, 1.0 - exp(-(center.variance * SSSR_DNSR_PREFILTER_VARIANCE_WEIGHT)));
    for (i32 i = 0; i < i32(sample_count); ++i) {
        let new_idx = group_thread_id + sample_offsets[i];
        let neighbor = load_from_group_shared_memory(new_idx);

        var weight = 1.0;
        weight *= get_edge_stopping_normal_weight(center.normal, neighbor.normal);
        weight *= get_edge_stopping_depth_weight(center.depth, neighbor.depth);
        weight *= get_radiance_weight(avg_radiance, neighbor.radiance.xyz, center.variance);
        weight *= variance_weight;

        // Accumulate all contributions.
        accumulated_weight += weight;
        accumulated_radiance += weight * neighbor.radiance.xyz;
        accumulated_variance += weight * weight * neighbor.variance;
    }

    accumulated_radiance /= accumulated_weight;
    accumulated_variance /= (accumulated_weight * accumulated_weight);
    resolved_radiance = accumulated_radiance;
    resolved_variance = accumulated_variance;
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

    let center_roughness = is_in_bounds ? sssr_roughness_to_alpha(roughness_image.Load(i32x3(dispatch_thread_id, 0))) : 1.0;
    initialize_group_shared_memory(dispatch_thread_id, group_thread_id, screen_size);
    GroupMemoryBarrierWithGroupSync();

    group_thread_id += 4; // Center threads in groupshared memory

    let center = load_from_group_shared_memory(group_thread_id);

    var resolved_radiance = center.radiance;
    var resolved_variance = center.variance;

    let needs_denoiser = center.variance > 0.0 && sssr_is_glossy_reflection(center_roughness, settings.roughness_threshold) && !sssr_is_mirror_reflection(center_roughness);
    if (needs_denoiser) {
        let uv8 = (f32x2(dispatch_thread_id) + 0.5) / f32x2((settings.render_size + 7u) / 8u * 8u);
        let avg_radiance = average_radiance_image.SampleLevel(linear_sampler, uv8, 0.0).xyz;
        resolve_neighborhood(group_thread_id, avg_radiance, center, resolved_radiance, resolved_variance);
    }

    if (is_in_bounds) {
        prefiltered_radiance_image[dispatch_thread_id] = f32x4(resolved_radiance, 0.0);
        prefiltered_variance_image[dispatch_thread_id] = resolved_variance;
    }
}
