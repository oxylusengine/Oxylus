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
import sky;
import passes.sssr.sssr_lib;
#if defined(SSSR_ENV_DDGI)
import ddgi;
#endif

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32x4> lit_scene_image;
[[vk::binding(2, 0)]] Texture2D<f32> depth_hierarchy;
[[vk::binding(3, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(4, 0)]] Texture2D<f32> roughness_image;
[[vk::binding(5, 0)]] Texture2D<f32x2> blue_noise_image;
[[vk::binding(6, 0)]] StructuredBuffer<u32> ray_list;
[[vk::binding(7, 0)]] StructuredBuffer<u32> ray_counters;
[[vk::binding(8, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> intersection_radiance_image;
[[vk::binding(9, 0)]] StorageImage2D<f32, ImageFormat::R8> confidence_image;
[[vk::binding(10, 0)]] Texture2D<f32> specular_occlusion_image;
[[vk::binding(11, 0)]] ConstantBuffer<HistogramLuminance> exposure_buffer;

#include <passes/sssr/env.slang>

#if defined(SSSR_ENV_SKY)
    #define SSSR_ATMOSPHERE_PARAMETER uniform Atmosphere *atmosphere,
    #define SSSR_ATMOSPHERE_ARGUMENT atmosphere,
#else
    #define SSSR_ATMOSPHERE_PARAMETER
    #define SSSR_ATMOSPHERE_ARGUMENT
#endif

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, SSSR_ATMOSPHERE_PARAMETER uniform SSSRSettings settings) -> void {
    let ray_index = thread_id.x;
    if (ray_index >= ray_counters[1]) {
        return;
    }

    u32x2 coords;
    bool copy_horizontal;
    bool copy_vertical;
    bool copy_diagonal;
    sssr_unpack_ray_coords(ray_list[ray_index], coords, copy_horizontal, copy_vertical, copy_diagonal);

    let screen_size = f32x2(settings.render_size);
    let texel = i32x2(coords);

    let uv = (f32x2(coords) + 0.5) / screen_size;

    let packed_normal = normal_image.Load(i32x3(texel, 0));
    let world_space_normal = normalize(com::oct_to_vec3(packed_normal.xy));
    let alpha = sssr_roughness_to_alpha(roughness_image.Load(i32x3(texel, 0)));
    let is_mirror = sssr_is_mirror_reflection(alpha);

    let most_detailed_mip = is_mirror ? 0 : i32(settings.most_detailed_mip);
    let mip_resolution = sssr_get_mip_resolution(screen_size, most_detailed_mip);
    let z = depth_hierarchy.Load(i32x3(i32x2(uv * mip_resolution), most_detailed_mip));

    let screen_uv_space_ray_origin = f32x3(uv, z);
    let view_space_ray = sssr_screen_to_view(camera, uv, z);
    let view_space_ray_direction = normalize(view_space_ray);

    let view_space_surface_normal = mul(camera.view, f32x4(world_space_normal, 0.0)).xyz;
    let view_space_reflected_direction = sssr_sample_reflection_vector(
        view_space_ray_direction,
        view_space_surface_normal,
        alpha,
        blue_noise_image.Load(i32x3(texel % 128, 0))
    );
    // projecting the unjittered view position keeps the jitter out of the difference
    let unjittered_origin = sssr_project_position(view_space_ray, camera.projection);
    let screen_space_ray_direction = sssr_project_direction(
        view_space_ray,
        view_space_reflected_direction,
        unjittered_origin,
        camera.projection
    );

    //====SSSR====
    bool valid_hit = false;
    f32 last_surface_z = 1.0;
    let hit = sssr_hierarchical_raymarch(
        depth_hierarchy,
        screen_uv_space_ray_origin,
        screen_space_ray_direction,
        is_mirror,
        screen_size,
        most_detailed_mip,
        settings.min_traversal_occupancy,
        settings.max_traversal_intersections,
        valid_hit,
        last_surface_z
    );

    // the hit stays in view space, where the precision is
    let view_space_hit = sssr_screen_to_view(camera, hit.xy, hit.z);
    let world_space_ray = mul(camera.inv_view, f32x4(view_space_hit - view_space_ray, 0.0)).xyz;

    // a reflected ray's vanishing point is usually off screen, so a ray through open sky leaves the frame rather than
    // stopping on a sky pixel, what it last crossed tells the two apart
    let is_sky = last_surface_z == 0.0;
    f32 confidence = 0.0;
    if (valid_hit) {
        confidence = sssr_validate_hit(
            depth_hierarchy,
            normal_image,
            camera,
            hit,
            uv,
            world_space_ray,
            screen_size,
            settings.depth_thickness
        );
    }

    let world_ray_length = max(0.0, length(world_space_ray));

    f32x3 reflection_radiance = f32x3(0.0);
    if (confidence > 0.0) {
        // Found an intersection with the depth buffer -> We can lookup the color from lit scene.
        let hit_texel = min(i32x2(screen_size * hit.xy), i32x2(screen_size) - 1);
        reflection_radiance = clamp(lit_scene_image.Load(i32x3(hit_texel, 0)).rgb, f32x3(0.0), f32x3(65000.0));
    }

    // Sample environment map.
    if (confidence < 1.0) {
        let world_space_reflected_direction = mul(camera.inv_view, f32x4(view_space_reflected_direction, 0.0)).xyz;
        let world_position = mul(camera.inv_view, f32x4(view_space_ray, 1.0)).xyz;
        let environment_lookup = sssr_ray_environment(
            settings,
            SSSR_ATMOSPHERE_ARGUMENT
            world_position,
            world_space_normal,
            normalize(com::oct_to_vec3(packed_normal.zw)),
            normalize(mul(camera.inv_view, f32x4(-view_space_ray, 0.0)).xyz),
            length(view_space_ray),
            world_space_reflected_direction,
            is_sky,
            texel
        );
        reflection_radiance = lerp(environment_lookup, reflection_radiance, confidence);
    }

    let exposure = sssr_current_exposure(settings, exposure_buffer.exposure);
    let new_sample = f32x4(exposure * reflection_radiance, world_ray_length);

    intersection_radiance_image[texel] = new_sample;
    confidence_image[texel] = confidence;

    let copy_target = coords ^ 1u; // Flip last bit to find the mirrored coords along the x and y axis within a quad.
    if (copy_horizontal) {
        let copy_coords = i32x2(u32x2(copy_target.x, coords.y));
        intersection_radiance_image[copy_coords] = new_sample;
        confidence_image[copy_coords] = confidence;
    }

    if (copy_vertical) {
        let copy_coords = i32x2(u32x2(coords.x, copy_target.y));
        intersection_radiance_image[copy_coords] = new_sample;
        confidence_image[copy_coords] = confidence;
    }

    if (copy_diagonal) {
        let copy_coords = i32x2(copy_target);
        intersection_radiance_image[copy_coords] = new_sample;
        confidence_image[copy_coords] = confidence;
    }
}
