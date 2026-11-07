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

module sssr_lib;

import common;
import gpu;
import scene;

public static const f32 SSSR_FLOAT_MAX = 3.402823466e+38;
public static const i32 SSSR_DEPTH_HIERARCHY_MAX_MIP = 6;

public func sssr_remap_for_wave_reduction(u32 lane) -> u32x2 {
    return u32x2((lane & 1u) | ((lane >> 2u) & 6u), ((lane >> 1u) & 3u) | ((lane >> 3u) & 4u));
}

// the roughness image holds perceptual roughness, every comparison and sampler below takes alpha
public func sssr_roughness_to_alpha(f32 perceptual_roughness) -> f32 {
    return perceptual_roughness * perceptual_roughness;
}

public func sssr_pack_ray_coords(u32x2 ray_coord, bool copy_horizontal, bool copy_vertical, bool copy_diagonal) -> u32 {
    let ray_x_15bit = ray_coord.x & 0x7fffu;
    let ray_y_14bit = ray_coord.y & 0x3fffu;
    let copy_horizontal_1bit = copy_horizontal ? 1u : 0u;
    let copy_vertical_1bit = copy_vertical ? 1u : 0u;
    let copy_diagonal_1bit = copy_diagonal ? 1u : 0u;

    return (copy_diagonal_1bit << 31u) | (copy_vertical_1bit << 30u) | (copy_horizontal_1bit << 29u) | (ray_y_14bit << 15u) | (ray_x_15bit << 0u);
}

public func sssr_unpack_ray_coords(
    u32 packed,
    out u32x2 ray_coord,
    out bool copy_horizontal,
    out bool copy_vertical,
    out bool copy_diagonal
) -> void {
    ray_coord.x = (packed >> 0u) & 32767u;
    ray_coord.y = (packed >> 15u) & 16383u;
    copy_horizontal = bool((packed >> 29u) & 1u);
    copy_vertical = bool((packed >> 30u) & 1u);
    copy_diagonal = bool((packed >> 31u) & 1u);
}

// ndc +y points down here, so unlike the reference there is no y flip
public func sssr_project_position(f32x3 origin, f32x4x4 mat) -> f32x3 {
    var projected = mul(mat, f32x4(origin, 1.0));
    projected.xyz /= projected.w;
    projected.xy = 0.5 * projected.xy + 0.5;
    return projected.xyz;
}

public func sssr_inv_project_position(f32x3 coord, f32x4x4 mat) -> f32x3 {
    coord.xy = 2.0 * coord.xy - 1.0;
    var projected = mul(mat, f32x4(coord, 1.0));
    projected.xyz /= projected.w;
    return projected.xyz;
}

public func sssr_project_direction(f32x3 origin, f32x3 direction, f32x3 screen_space_origin, f32x4x4 mat) -> f32x3 {
    // a direction that would take w to 0 or below is shortened so the endpoint stays in front of the camera
    let clip_origin = mul(mat, f32x4(origin, 1.0));
    let clip_direction = mul(mat, f32x4(direction, 0.0));
    if (clip_direction.w < 0.0) {
        direction *= min(1.0, 0.5 * clip_origin.w / -clip_direction.w);
    }

    let offsetted = sssr_project_position(origin + direction, mat);
    return offsetted - screen_space_origin;
}

public func sssr_is_glossy_reflection(f32 alpha, f32 roughness_threshold) -> bool {
    return alpha < roughness_threshold;
}

public func sssr_is_mirror_reflection(f32 alpha) -> bool {
    return alpha < 0.0001;
}

// geometry is rasterized with the jitter added to clip xy, so a raster pixel really sits at uv - jitter / resolution
public func sssr_screen_to_view(in Camera camera, f32x2 uv, f32 z) -> f32x3 {
    let unjittered_uv = uv - camera.temporalaa_jitter / camera.resolution;
    return sssr_inv_project_position(f32x3(unjittered_uv, z), camera.inv_projection);
}

// http://jcgt.org/published/0007/04/01/paper.pdf by Eric Heitz
// Input Ve: view direction
// Input alpha_x, alpha_y: roughness parameters
// Input U1, U2: uniform random numbers
// Output Ne: normal sampled with PDF D_Ve(Ne) = G1(Ve) * max(0, dot(Ve, Ne)) * D(Ne) / Ve.z
public func sssr_sample_ggx_vndf(f32x3 Ve, f32 alpha_x, f32 alpha_y, f32 U1, f32 U2) -> f32x3 {
    // Section 3.2: transforming the view direction to the hemisphere configuration
    let Vh = normalize(f32x3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis (with special case if cross product is zero)
    let lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    let T1 = lensq > 0.0 ? f32x3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : f32x3(1.0, 0.0, 0.0);
    let T2 = cross(Vh, T1);
    // Section 4.2: parameterization of the projected area
    let r = sqrt(U1);
    let phi = 2.0 * PI * U2;
    let t1 = r * cos(phi);
    var t2 = r * sin(phi);
    let s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
    // Section 4.3: reprojection onto hemisphere
    let Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    // Section 3.4: transforming the normal back to the ellipsoid configuration
    return normalize(f32x3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0, Nh.z)));
}

public func sssr_sample_ggx_vndf_ellipsoid(f32x3 Ve, f32 alpha_x, f32 alpha_y, f32 U1, f32 U2) -> f32x3 {
    return sssr_sample_ggx_vndf(Ve, alpha_x, alpha_y, U1, U2);
}

public func sssr_sample_ggx_vndf_hemisphere(f32x3 Ve, f32 alpha, f32 U1, f32 U2) -> f32x3 {
    return sssr_sample_ggx_vndf_ellipsoid(Ve, alpha, alpha, U1, U2);
}

public func sssr_sample_reflection_vector(f32x3 view_direction, f32x3 normal, f32 alpha, f32x2 u) -> f32x3 {
    f32x3 U;
    let N = normal;
    if (abs(N.z) > 0.0) {
        let k = sqrt(N.y * N.y + N.z * N.z);
        U.x = 0.0; U.y = -N.z / k; U.z = N.y / k;
    } else {
        let k = sqrt(N.x * N.x + N.y * N.y);
        U.x = N.y / k; U.y = -N.x / k; U.z = 0.0;
    }

    // TBN 3x3 matrix
    let TBN_row0 = U;
    let TBN_row1 = cross(N, U);
    let TBN_row2 = N;

    // TBN * -view_direction
    let view_direction_tbn = f32x3(dot(TBN_row0, -view_direction), dot(TBN_row1, -view_direction), dot(TBN_row2, -view_direction));

    let sampled_normal_tbn = sssr_sample_ggx_vndf_hemisphere(view_direction_tbn, alpha, u.x, u.y);
    let reflected_direction_tbn = reflect(-view_direction_tbn, sampled_normal_tbn);

    // Transpose of TBN
    let TBN_col0 = f32x3(TBN_row0[0], TBN_row1[0], TBN_row2[0]);
    let TBN_col1 = f32x3(TBN_row0[1], TBN_row1[1], TBN_row2[1]);
    let TBN_col2 = f32x3(TBN_row0[2], TBN_row1[2], TBN_row2[2]);

    // transpose(TBN) * reflected_direction_tbn
    let reflected = f32x3(dot(TBN_col0, reflected_direction_tbn), dot(TBN_col1, reflected_direction_tbn), dot(TBN_col2, reflected_direction_tbn));

    // a normal facing away from the viewer makes the sampled normal degenerate, fall back to the mirror direction
    if (!all(isfinite(reflected))) {
        return reflect(view_direction, N);
    }

    return reflected;
}

public func sssr_initial_advance_ray(
    f32x3 origin,
    f32x3 direction,
    f32x3 inv_direction,
    f32x2 current_mip_resolution,
    f32x2 current_mip_resolution_inv,
    f32x2 floor_offset,
    f32x2 uv_offset,
    out f32x3 position,
    out f32 current_t
) -> void {
    let current_mip_position = current_mip_resolution * origin.xy;

    // Intersect ray with the half box that is pointing away from the ray origin.
    var xy_plane = floor(current_mip_position) + floor_offset;
    xy_plane = xy_plane * current_mip_resolution_inv + uv_offset;

    // o + d * t = p' => t = (p' - o) / d
    let t = xy_plane * inv_direction.xy - origin.xy * inv_direction.xy;
    current_t = min(t.x, t.y);
    position = origin + current_t * direction;
}

public func sssr_advance_ray(
    f32x3 origin,
    f32x3 direction,
    f32x3 inv_direction,
    f32x2 current_mip_position,
    f32x2 current_mip_resolution_inv,
    f32x2 floor_offset,
    f32x2 uv_offset,
    f32 surface_z,
    inout f32x3 position,
    inout f32 current_t
) -> bool {
    // Create boundary planes
    var xy_plane = floor(current_mip_position) + floor_offset;
    xy_plane = xy_plane * current_mip_resolution_inv + uv_offset;
    let boundary_planes = f32x3(xy_plane, surface_z);

    // Intersect ray with the half box that is pointing away from the ray origin.
    // o + d * t = p' => t = (p' - o) / d
    var t = boundary_planes * inv_direction - origin * inv_direction;

    // Prevent using z plane when shooting out of the depth buffer.
    t.z = direction.z < 0.0 ? t.z : SSSR_FLOAT_MAX;

    // Choose nearest intersection with a boundary.
    let t_min = min(min(t.x, t.y), t.z);

    // Larger z means closer to the camera.
    let above_surface = surface_z < position.z;

    // Decide whether we are able to advance the ray until we hit the xy boundaries or if we had to clamp it at the surface.
    // We use the asuint comparison to avoid NaN / Inf logic, also we actually care about bitwise equality here to see if t_min is the t.z we fed into the min3 above.
    let skipped_tile = asuint(t_min) != asuint(t.z) && above_surface;

    // Make sure to only advance the ray if we're still above the surface.
    current_t = above_surface ? t_min : current_t;

    // Advance ray
    position = origin + current_t * direction;

    return skipped_tile;
}

public func sssr_get_mip_resolution(f32x2 screen_dimensions, i32 mip_level) -> f32x2 {
    return screen_dimensions * pow(0.5, f32(mip_level));
}

// Requires origin and direction of the ray to be in screen space [0, 1] x [0, 1]
public func sssr_hierarchical_raymarch(
    Texture2D<f32> depth_hierarchy,
    f32x3 origin,
    f32x3 direction,
    bool is_mirror,
    f32x2 screen_size,
    i32 most_detailed_mip,
    u32 min_traversal_occupancy,
    u32 max_traversal_intersections,
    out bool valid_hit,
    out f32 last_surface_z
) -> f32x3 {
    let inv_direction = select(direction != f32x3(0.0), f32x3(1.0) / direction, f32x3(SSSR_FLOAT_MAX));

    // Start on mip with highest detail.
    var current_mip = most_detailed_mip;

    // Could recompute these every iteration, but it's faster to hoist them out and update them.
    var current_mip_resolution = sssr_get_mip_resolution(screen_size, current_mip);
    var current_mip_resolution_inv = rcp(current_mip_resolution);

    // Offset to the bounding boxes uv space to intersect the ray with the center of the next pixel.
    // This means we ever so slightly over shoot into the next region.
    var uv_offset = 0.005 * exp2(f32(most_detailed_mip)) / screen_size;
    uv_offset.x = direction.x < 0.0 ? -uv_offset.x : uv_offset.x;
    uv_offset.y = direction.y < 0.0 ? -uv_offset.y : uv_offset.y;

    // Offset applied depending on current mip resolution to move the boundary to the left/right upper/lower border depending on ray direction.
    f32x2 floor_offset;
    floor_offset.x = direction.x < 0.0 ? 0.0 : 1.0;
    floor_offset.y = direction.y < 0.0 ? 0.0 : 1.0;

    // Initially advance ray to avoid immediate self intersections.
    f32 current_t;
    f32x3 position;
    sssr_initial_advance_ray(origin, direction, inv_direction, current_mip_resolution, current_mip_resolution_inv, floor_offset, uv_offset, position, current_t);

    // a load outside the image is undefined, so leaving the screen ends the traversal
    var is_on_screen = all(position.xy >= 0.0) && all(position.xy < 1.0);
    bool exit_due_to_low_occupancy = false;
    i32 i = 0;
    while (u32(i) < max_traversal_intersections && current_mip >= most_detailed_mip && !exit_due_to_low_occupancy && is_on_screen) {
        let current_mip_position = current_mip_resolution * position.xy;
        let surface_z = depth_hierarchy.Load(i32x3(i32x2(current_mip_position), current_mip));
        exit_due_to_low_occupancy = !is_mirror && WaveActiveCountBits(true) <= min_traversal_occupancy;
        let skipped_tile = sssr_advance_ray(origin, direction, inv_direction, current_mip_position, current_mip_resolution_inv, floor_offset, uv_offset, surface_z, position, current_t);

        // Don't increase mip further than this because we did not generate it
        let next_mip_is_out_of_range = skipped_tile && (current_mip >= SSSR_DEPTH_HIERARCHY_MAX_MIP);
        if (!next_mip_is_out_of_range) {
            current_mip += skipped_tile ? 1 : -1;
            current_mip_resolution *= skipped_tile ? 0.5 : 2.0;
            current_mip_resolution_inv *= skipped_tile ? 2.0 : 0.5;
        }

        ++i;
        is_on_screen = all(position.xy >= 0.0) && all(position.xy < 1.0);
    }

    valid_hit = (u32(i) <= max_traversal_intersections) && is_on_screen;

    // coarse cells can still hold geometry and the last step can be far from the edge, so look at the finest mip where
    // the ray actually leaves the screen
    var exit_position = position.xy;
    if (!is_on_screen) {
        let exit_t = (floor_offset - origin.xy) * inv_direction.xy;
        exit_position = origin.xy + direction.xy * min(exit_t.x, exit_t.y);
    }

    let exit_texel = clamp(i32x2(screen_size * exit_position), i32x2(0), i32x2(screen_size) - 1);
    last_surface_z = depth_hierarchy.Load(i32x3(exit_texel, 0));

    return position;
}

public func sssr_validate_hit(
    Texture2D<f32> depth_hierarchy,
    Texture2D<f32x4> normal_image,
    in Camera camera,
    f32x3 hit,
    f32x2 uv,
    f32x3 world_space_ray_direction,
    f32x2 screen_size,
    f32 depth_buffer_thickness
) -> f32 {
    // Reject hits outside the view frustum
    if ((hit.x < 0.0) || (hit.y < 0.0) || (hit.x > 1.0) || (hit.y > 1.0)) {
        return 0.0;
    }

    // Reject the hit if we didnt advance the ray significantly to avoid immediate self reflection
    let manhattan_dist = abs(hit.xy - uv);
    if ((manhattan_dist.x < (2.0 / screen_size.x)) && (manhattan_dist.y < (2.0 / screen_size.y))) {
        return 0.0;
    }

    // a hit on the right or bottom edge would index one texel past the image
    let texel_coords = min(i32x2(screen_size * hit.xy), i32x2(screen_size) - 1);

    // Don't lookup radiance from the background.
    let surface_z = depth_hierarchy.Load(i32x3(texel_coords / 2, 1));
    if (surface_z == 0.0) {
        return 0.0;
    }

    // We check if we hit the surface from the back, these should be rejected.
    let hit_normal = normalize(com::oct_to_vec3(normal_image.Load(i32x3(texel_coords, 0)).xy));
    if (dot(hit_normal, world_space_ray_direction) > 0.0) {
        return 0.0;
    }

    let view_space_surface = sssr_screen_to_view(camera, hit.xy, surface_z);
    let view_space_hit = sssr_screen_to_view(camera, hit.xy, hit.z);
    let distance = length(view_space_surface - view_space_hit);

    // Fade out hits near the screen borders
    let fov = 0.05 * f32x2(screen_size.y / screen_size.x, 1.0);
    let border = smoothstep(f32x2(0.0), fov, hit.xy) * (1.0 - smoothstep(f32x2(1.0) - fov, f32x2(1.0), hit.xy));
    let vignette = border.x * border.y;

    // We accept all hits that are within a reasonable minimum distance below the surface.
    // Add constant in linear space to avoid growing of the reflections toward the reflected objects.
    var confidence = 1.0 - smoothstep(0.0, depth_buffer_thickness, distance);
    confidence *= confidence;

    return vignette * confidence;
}

public static const f32 SSSR_DNSR_GAUSSIAN_K = 3.0;
public static const f32 SSSR_DNSR_RADIANCE_WEIGHT_BIAS = 0.6;
public static const f32 SSSR_DNSR_RADIANCE_WEIGHT_VARIANCE_K = 0.1;
public static const f32 SSSR_DNSR_AVG_RADIANCE_LUMINANCE_WEIGHT = 0.3;
public static const f32 SSSR_DNSR_PREFILTER_VARIANCE_WEIGHT = 4.4;
public static const f32 SSSR_DNSR_REPROJECT_SURFACE_DISCARD_VARIANCE_WEIGHT = 1.5;
public static const f32 SSSR_DNSR_PREFILTER_VARIANCE_BIAS = 0.1;
public static const f32 SSSR_DNSR_PREFILTER_NORMAL_SIGMA = 512.0;
public static const f32 SSSR_DNSR_PREFILTER_DEPTH_SIGMA = 4.0;
public static const f32 SSSR_DNSR_DISOCCLUSION_NORMAL_WEIGHT = 1.4;
public static const f32 SSSR_DNSR_DISOCCLUSION_DEPTH_WEIGHT = 1.0;
public static const f32 SSSR_DNSR_DISOCCLUSION_THRESHOLD = 0.9;
public static const f32 SSSR_DNSR_REPROJECTION_NORMAL_SIMILARITY_THRESHOLD = 0.9999;
public static const i32 SSSR_DNSR_LOCAL_NEIGHBORHOOD_RADIUS = 4;
public static const i32 SSSR_DNSR_MAX_SAMPLES = 32;
public static const f32 SSSR_DNSR_RADIANCE_THRESHOLD = 0.0001;

public func sssr_dnsr_samples_for_roughness(f32 r) -> f32 {
    return 1.0 - exp(-r * 100.0);
}

// the radiance the denoiser sees is radiance * exposure, its constants are tuned for display range values
public func sssr_current_exposure(in SSSRSettings settings, f32 buffered_exposure) -> f32 {
    let exposure = settings.is_eye_adapted != 0 ? buffered_exposure : settings.fixed_exposure;
    return (isfinite(exposure) && exposure > 0.0) ? exposure : 1.0;
}

// cleared history and sky texels hold zero, which must stay zero instead of turning into nan
public func sssr_safe_normalize(f32x3 v) -> f32x3 {
    let len_sq = dot(v, v);
    return len_sq > 0.0 ? v * rsqrt(len_sq) : f32x3(0.0);
}

// depth 0 is the far plane, so sky sits at infinity
public func sssr_dnsr_linear_depth(in Camera camera, f32x2 uv, f32 depth) -> f32 {
    if (depth == 0.0) {
        return 1.0e6;
    }

    return abs(sssr_inv_project_position(f32x3(uv, depth), camera.inv_projection).z);
}

public func sssr_dnsr_luminance(f32x3 color) -> f32 {
    return max(dot(color, f32x3(0.299, 0.587, 0.114)), 0.001);
}

public func sssr_dnsr_compute_temporal_variance(f32x3 history_radiance, f32x3 radiance) -> f32 {
    let history_luminance = sssr_dnsr_luminance(history_radiance);
    let luminance = sssr_dnsr_luminance(radiance);
    let diff = abs(history_luminance - luminance) / max(max(history_luminance, luminance), 0.5);
    return diff * diff;
}

public func sssr_dnsr_local_neighborhood_kernel_weight(f32 i) -> f32 {
    let radius = f32(SSSR_DNSR_LOCAL_NEIGHBORHOOD_RADIUS) + 1.0;
    return exp(-SSSR_DNSR_GAUSSIAN_K * (i * i) / (radius * radius));
}

/**********************************************************************
Copyright (c) [2015] [Playdead]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
********************************************************************/
public func sssr_dnsr_clip_aabb(f32x3 aabb_min, f32x3 aabb_max, f32x3 prev_sample) -> f32x3 {
    // Main idea behind clipping - it prevents clustering when neighbor color space
    // is distant from history sample

    // Here we find intersection between color vector and aabb color box

    // Note: only clips towards aabb center
    let aabb_center = 0.5 * (aabb_max + aabb_min);
    let extent_clip = 0.5 * (aabb_max - aabb_min) + 0.001;

    // Find color vector
    let color_vector = prev_sample - aabb_center;
    // Transform into clip space
    var color_vector_clip = color_vector / extent_clip;
    // Find max absolute component
    color_vector_clip = abs(color_vector_clip);
    let max_abs_unit = max(max(color_vector_clip.x, color_vector_clip.y), color_vector_clip.z);

    if (max_abs_unit > 1.0) {
        return aabb_center + color_vector / max_abs_unit; // clip towards color vector
    } else {
        return prev_sample; // point is inside aabb
    }
}
