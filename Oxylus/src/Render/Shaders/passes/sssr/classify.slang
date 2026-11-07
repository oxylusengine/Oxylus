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
import pbr;
import scene;
import passes.sssr.sssr_lib;
#if defined(SSSR_ENV_DDGI)
import ddgi;
#endif

#define SSSR_CLASSIFY

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(3, 0)]] Texture2D<f32x2> metallic_roughness_image;
[[vk::binding(4, 0)]] Texture2D<f32x4> albedo_image;
[[vk::binding(5, 0)]] Texture2D<f32> gtao_image;
[[vk::binding(6, 0)]] RWStructuredBuffer<u32> ray_counters;
[[vk::binding(7, 0)]] RWStructuredBuffer<u32> ray_list;
[[vk::binding(8, 0)]] RWStructuredBuffer<u32> tile_list;
[[vk::binding(9, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> intersection_radiance_image;
[[vk::binding(10, 0)]] StorageImage2D<f32, ImageFormat::R8> confidence_image;
[[vk::binding(11, 0)]] StorageImage2D<f32, ImageFormat::R8> roughness_image;
[[vk::binding(12, 0)]] Texture2D<f32> previous_variance_image;
[[vk::binding(13, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> normal_history_image;
[[vk::binding(14, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> radiance_history_image;
[[vk::binding(15, 0)]] StorageImage2D<f32, ImageFormat::R16F> variance_history_image;
[[vk::binding(16, 0)]] StorageImage2D<f32, ImageFormat::R16F> sample_count_history_image;
[[vk::binding(17, 0)]] StorageImage2D<f32, ImageFormat::R8> specular_occlusion_image;
[[vk::binding(18, 0)]] ConstantBuffer<HistogramLuminance> exposure_buffer;

#include <passes/sssr/env.slang>

func sssr_is_base_ray(u32x2 dispatch_thread_id, u32 samples_per_quad) -> bool {
    switch (samples_per_quad) {
        case 1: {
            return ((dispatch_thread_id.x & 1u) | (dispatch_thread_id.y & 1u)) == 0u; // Deactivates 3 out of 4 rays
        }
        case 2: {
            return (dispatch_thread_id.x & 1u) == (dispatch_thread_id.y & 1u); // Deactivates 2 out of 4 rays. Keeps diagonal.
        }
        default: { // case 4:
            return true;
        }
    }
}

groupshared u32 g_tile_count;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 group_id : SV_GroupID, uniform SSSRSettings settings) -> void {
    // quads are four consecutive lanes whatever the wave size, so lane ^ 1 is always the quad partner
    let local_id = com::subgroup_id() * WaveGetLaneCount() + WaveGetLaneIndex();
    let group_thread_id = sssr_remap_for_wave_reduction(local_id);
    let dispatch_thread_id = group_id.xy * 8 + group_thread_id;
    let texel = i32x2(dispatch_thread_id);

    if (local_id == 0) {
        g_tile_count = 0;
    }

    let is_first_lane_of_wave = WaveIsFirstLane();

    // Disable offscreen pixels, a load outside the image is undefined
    let is_in_bounds = all(dispatch_thread_id < settings.render_size);
    var depth = 0.0;
    var perceptual_roughness = 0.0;
    var packed_normal = f32x4(0.0);
    var occlusion = 1.0;
    if (is_in_bounds) {
        depth = depth_image.Load(i32x3(texel, 0));
        perceptual_roughness = metallic_roughness_image.Load(i32x3(texel, 0)).g;
        packed_normal = normal_image.Load(i32x3(texel, 0));
        occlusion = albedo_image.Load(i32x3(texel, 0)).a * gtao_image.Load(i32x3(texel, 0)).r;
    }

    // First we figure out on a per thread basis if we need to shoot a reflection ray.
    // Dont shoot a ray on very rough surfaces.
    let alpha = sssr_roughness_to_alpha(perceptual_roughness);
    let is_reflective_surface = depth > 0.0;
    let is_glossy_reflection = sssr_is_glossy_reflection(alpha, settings.roughness_threshold);
    var needs_ray = is_in_bounds && is_glossy_reflection && is_reflective_surface;

    // Also we dont need to run the denoiser on mirror reflections.
    let needs_denoiser = needs_ray && !sssr_is_mirror_reflection(alpha);

    // Decide which ray to keep
    let is_base_ray = sssr_is_base_ray(dispatch_thread_id, settings.samples_per_quad);

    // a glossy pixel whose base ray is not traced would get zero radiance and the temporal early out would keep it
    // there, so it traces for itself
    let source_lane = settings.samples_per_quad == 1 ? (WaveGetLaneIndex() & ~3u) : (WaveGetLaneIndex() ^ 1u);
    let is_source_tracing = WaveReadLaneAt((is_reflective_surface && is_glossy_reflection) ? 1u : 0u, source_lane) != 0u;
    needs_ray = needs_ray && (!needs_denoiser || is_base_ray || !is_source_tracing); // Make sure to not deactivate mirror reflection rays.

    if (settings.is_temporal_variance_guided != 0 && needs_denoiser && !needs_ray) {
        let has_temporal_variance = previous_variance_image.Load(i32x3(texel, 0)) > settings.variance_threshold;
        needs_ray = needs_ray || has_temporal_variance;
    }

    GroupMemoryBarrierWithGroupSync(); // Wait until g_tile_count is cleared - allow some computations before and after

    // Now we know for each thread if it needs to shoot a ray and wether or not a denoiser pass has to run on this pixel.

    if (is_glossy_reflection && is_reflective_surface) {
        InterlockedAdd(g_tile_count, 1);
    }

    // Next we have to figure out for which pixels that ray is creating the values for. Thus, if we have to copy its value horizontal, vertical or across.
    let require_copy = (!needs_ray && needs_denoiser) ? 1u : 0u; // Our pixel only requires a copy if we want to run a denoiser on it but don't want to shoot a ray for it.
    let copy_horizontal = com::wave_shuffle_xor(require_copy, 1u) != 0u && (settings.samples_per_quad != 4) && is_base_ray; // QuadReadAcrossX
    let copy_vertical = com::wave_shuffle_xor(require_copy, 2u) != 0u && (settings.samples_per_quad == 1) && is_base_ray; // QuadReadAcrossY
    let copy_diagonal = com::wave_shuffle_xor(require_copy, 3u) != 0u && (settings.samples_per_quad == 1) && is_base_ray; // QuadReadAcrossDiagonal

    // Thus, we need to compact the rays and append them all at once to the ray list.
    let local_ray_index_in_wave = WavePrefixCountBits(needs_ray);
    let wave_ray_count = WaveActiveCountBits(needs_ray);
    u32 base_ray_index = 0;
    if (is_first_lane_of_wave) {
        InterlockedAdd(ray_counters[0], wave_ray_count, base_ray_index);
    }

    base_ray_index = WaveReadLaneFirst(base_ray_index);
    if (needs_ray) {
        ray_list[base_ray_index + local_ray_index_in_wave] = sssr_pack_ray_coords(dispatch_thread_id, copy_horizontal, copy_vertical, copy_diagonal);
    }

    let world_space_normal = normalize(com::oct_to_vec3(packed_normal.xy));
    var specular_occlusion = 1.0;
    f32x4 intersection_output = f32x4(0.0);
    if (is_reflective_surface) {
        let uv = (f32x2(dispatch_thread_id) + 0.5) / f32x2(settings.render_size);
        let view_space_ray = sssr_screen_to_view(camera, uv, depth);
        let view_space_ray_direction = normalize(view_space_ray);
        let view_direction = normalize(mul(camera.inv_view, f32x4(-view_space_ray, 0.0)).xyz);
        let NoV = abs(dot(world_space_normal, view_direction)) + 1e-5;
        let clamped_roughness = clamp(perceptual_roughness, 0.045, 1.0);
        specular_occlusion = specular_occlusion_horizon(NoV, occlusion, clamped_roughness);

        if (!is_glossy_reflection) {
            // Fall back to environment map without preparing a ray
            let view_space_surface_normal = mul(camera.view, f32x4(world_space_normal, 0.0)).xyz;
            let view_space_reflected_direction = reflect(view_space_ray_direction, view_space_surface_normal);
            let world_space_reflected_direction = mul(camera.inv_view, f32x4(view_space_reflected_direction, 0.0)).xyz;
            let world_position = mul(camera.inv_view, f32x4(view_space_ray, 1.0)).xyz;
            let geometric_normal = normalize(com::oct_to_vec3(packed_normal.zw));

            intersection_output.xyz = specular_occlusion * sssr_rough_environment(
                settings,
                world_position,
                world_space_normal,
                geometric_normal,
                view_direction,
                length(view_space_ray),
                world_space_reflected_direction,
                clamped_roughness
            );
        }
    }

    if (is_in_bounds) {
        let exposure = sssr_current_exposure(settings, exposure_buffer.exposure);
        intersection_radiance_image[texel] = f32x4(exposure * intersection_output.xyz, intersection_output.w);
        confidence_image[texel] = 0.0;
        roughness_image[texel] = is_reflective_surface ? perceptual_roughness : 1.0;
        normal_history_image[texel] = f32x4(world_space_normal, 0.0);
        specular_occlusion_image[texel] = specular_occlusion;

        // pixels the denoiser leaves alone hold their final value already, the others are written by it, history stays
        // raw so it never needs an exposure ratio
        if (!needs_denoiser) {
            radiance_history_image[texel] = intersection_output;
            variance_history_image[texel] = 0.0;
            sample_count_history_image[texel] = 0.0;
        }
    }

    GroupMemoryBarrierWithGroupSync(); // Wait until g_tile_count

    if (local_id == 0 && g_tile_count > 0) {
        u32 tile_offset;
        InterlockedAdd(ray_counters[2], 1, tile_offset);
        let tile_coord = group_id.xy * 8;
        tile_list[tile_offset] = ((tile_coord.y & 0xffffu) << 16) | ((tile_coord.x & 0xffffu) << 0);
    }
}
