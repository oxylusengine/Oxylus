// Visibility Bitmask Ground-Truth Ambient Occlusion (VBGTAO)
// Credits:
// - https://ar5iv.labs.arxiv.org/html/2301.11376
// - https://github.com/bevyengine/bevy/blob/main/crates/bevy_pbr/src/ssao/ssao.wgsl

import common;
import gpu;
import scene;

constexpr static let SECTOR_COUNT = 32u;

struct ShaderParameters {
    ConstantBuffer<Camera> camera;
    Texture2D<f32> prefiltered_depth;
    Texture2D<float4> normals;
    Texture2D<u16> hilbert_noise;
    RWTexture2D<f32> ambient_occlusion;
    RWTexture2D<u32> depth_differences;
    SamplerState point_clamp_sampler;
    SamplerState linear_clamp_sampler;
};

f32 fast_sqrt(f32 x) {
    return (asfloat( 0x1fbd1df5 + ( asint( x ) >> 1 ) ));
}
// input [-1, 1] and output [0, PI], from https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
f32 fast_acos(f32 inX) {
    f32 x = abs(inX);
    f32 res = -0.156583 * x + HALF_PI;
    res *= fast_sqrt(1.0 - x);
    return (inX >= 0) ? res : PI - res;
}

func load_noise(u32x2 coord, in Texture2D<u16> hilbert_noise, u32 noise_index) -> f32x2{
    var index = (u32)hilbert_noise.Load(i32x3(coord % 64, 0)).r;
    index += 288 * (noise_index % 64);

    // R2 sequence - see http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
    return f32x2(frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114)));
}

func load_normal_view_space(
    in f32x2 uv,
    in Camera camera,
    in Texture2D<f32x4> normals,
    in SamplerState sampler_point_clamp
) -> f32x3 {
    let packed_normals = normals.SampleLevel(sampler_point_clamp, uv, 0.0);
    var world_normal = com::oct_to_vec3(packed_normals.zw);
    world_normal = mul(camera.view, f32x4(world_normal, 0.0)).xyz;
    return normalize(world_normal);
}

func reconstruct_view_space_position(
    f32x2 uv,
    f32 depth,
    in Camera camera
) -> f32x3 {
    let clip_xy = f32x2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    let t = mul(camera.inv_projection, f32x4(clip_xy, depth, 1.0));
    let view_xyz = t.xyz / t.w;
    return view_xyz;
}

func calculate_edges(
    i32x2 pixel_coord,
    in f32x2 resolution,
    in Texture2D<f32> src_depth,
    in SamplerState sampler_point_clamp,
    in RWTexture2D<u32> edges
) -> f32 {
    let uv = f32x2(pixel_coord) / resolution;
    let depths_upper_left = src_depth.GatherRed(sampler_point_clamp, uv);
    let depths_bottom_right = src_depth.GatherRed(sampler_point_clamp, uv, i32x2(1, 1));
    let depth_center = depths_upper_left.y;
    let depth_left = depths_upper_left.x;
    let depth_top = depths_upper_left.z;
    let depth_bottom = depths_bottom_right.x;
    let depth_right = depths_bottom_right.z;

    var edge_info = f32x4(depth_left, depth_right, depth_top, depth_bottom) - depth_center;
    let slope_left_right = (edge_info.y - edge_info.x) * 0.5;
    let slope_top_bottom = (edge_info.w - edge_info.z) * 0.5;
    let edge_info_slope_adjusted = edge_info + f32x4(slope_left_right, -slope_left_right, slope_top_bottom, -slope_top_bottom);
    edge_info = min(abs(edge_info), abs(edge_info_slope_adjusted));
    let bias = 0.25; // Using the bias and then saturating nudges the values a bit
    let scale = depth_center * 0.011; // Weight the edges by their distance from the camera
    edge_info = saturate((1.0 + bias) - edge_info / scale); // Apply the bias and scale, and invert edge_info so that small values become large, and vice versa

    // Pack the edge info into the texture
    let edge_info_packed = packUnorm4x8(edge_info);
    edges[pixel_coord] = edge_info_packed;

    return depth_center;
}

func updateSectors(
    f32 min_horizon,
    f32 max_horizon,
    f32 samples_per_slice,
    u32 bitmask,
) -> u32 {
    let start_horizon = u32(min_horizon * samples_per_slice);
    let angle_horizon = u32(ceil((max_horizon - min_horizon) * samples_per_slice));

    return bitfieldInsert(bitmask, 0xFFFFFFFFu, start_horizon, angle_horizon);
}

func updateSectors(
    f32 min_horizon,
    f32 max_horizon,
    u32 bitmask,
) -> u32 {
    let startHorizonInt = uint(min_horizon * float(SECTOR_COUNT));
    // ceil: Sample needs to at least touch a sector to activate it
    // round: Sample needs to cover at least half a sector to activate it
    // floor: Sample needs to cover the entire sector to activate it
    let angleHorizonInt = uint(ceil(saturate(max_horizon - min_horizon) * float(SECTOR_COUNT)));
    let angleHorizonBitfield = angleHorizonInt > 0u ? (0xFFFFFFFFu >> (SECTOR_COUNT-angleHorizonInt)) : 0u;
    let currentOccludedBitfield = angleHorizonBitfield << startHorizonInt;
    return bitmask | currentOccludedBitfield;
}

func calc_visibility_mask(
    u32 bitmask,
    f32 thickness,
    f32x3 delta_position,
    f32x3 view_dir,
    f32 sampling_direction,
    f32x2 n,
) -> u32 {
    let delta_position_back_face = delta_position - view_dir * thickness;

    var front_back_horizon = f32x2(
        fast_acos(dot(normalize(delta_position), view_dir)),
        fast_acos(dot(normalize(delta_position_back_face), view_dir)),
    );

    front_back_horizon = saturate(((sampling_direction * -front_back_horizon) + n + HALF_PI) / PI);
    front_back_horizon = select(sampling_direction >= 0.0, front_back_horizon.yx, front_back_horizon.xy);

    return updateSectors(front_back_horizon.x, front_back_horizon.y, bitmask);
}

[[numthreads(16, 16, 1)]]
func cs_main(
    const uint2 pixel_coordinates : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform VBGTAOSettings settings
) -> void {
    let thickness = settings.thickness;
    var slice_count = settings.slice_count;
    var samples_per_slice_side = settings.samples_per_slice_side;
    let effect_radius = settings.effect_radius * 1.457;
    let falloff_range = 0.615 * effect_radius;
    let falloff_from = effect_radius * (1.0 - 0.615);
    let falloff_mul = -1.0 / falloff_range;
    let falloff_add = falloff_from / falloff_range + 1.0;
    let mip_sampling_offset = 3.30;  // TODO: Should probably add this to settings

    let uv = (f32x2(pixel_coordinates) + 0.5) / params.camera.resolution;
    var pixel_depth = calculate_edges(
        pixel_coordinates, params.camera.resolution, params.prefiltered_depth, params.point_clamp_sampler, params.depth_differences);
    pixel_depth += 0.00001;
    let origin = reconstruct_view_space_position(uv, pixel_depth, params.camera);
    let view_dir = normalize(-origin);
    let normal = load_normal_view_space(uv, params.camera, params.normals, params.point_clamp_sampler);

    let noise = load_noise(pixel_coordinates, params.hilbert_noise, settings.noise_index);
    let sample_scale = -(0.5 * effect_radius * params.camera.projection[0][0]) / origin.z;

    var visibility = 0.0;
    for (var slice_t = 0.0; slice_t < slice_count; slice_t += 1.0) {
        let slice = (slice_t + noise.x) / slice_count;
        let phi = slice * PI;
        let omega = f32x2(cos(phi), sin(phi));

        let direction = f32x3(omega.xy, 0.0);
        let orthographic_direction = normalize(direction - (dot(direction, view_dir) * view_dir));
        let axis = cross(direction, view_dir);
        let projected_normal = normal - axis * dot(normal, axis);
        let projected_normal_length = length(projected_normal);

        let sign_norm = sign(dot(orthographic_direction, projected_normal));
        let cos_norm = saturate(dot(projected_normal, normal) / projected_normal_length);
        let n = sign_norm * fast_acos(cos_norm);

        var bitmask = 0u;
        let sample_mul = f32x2(omega.x, -omega.y) * sample_scale;
        for (var sample_t = 0.0; sample_t < samples_per_slice_side; sample_t += 1.0) {
            var sample = (slice + sample_t * samples_per_slice_side) * 0.6180339887498948482;
            sample = fract(noise.y + sample);

            var s  = (sample_t + sample) / samples_per_slice_side;
            s *= s;

            let sample_offset = s * sample_mul;
            let sample_screen_pos_1 = uv + sample_offset;
            let sample_screen_pos_2 = uv - sample_offset;

            let sample_mip_level = clamp(log2(length(sample_offset * params.camera.resolution)) - mip_sampling_offset, 0.0, 5.0);
            let depth_1 = params.prefiltered_depth.SampleLevel(params.linear_clamp_sampler, sample_screen_pos_1, sample_mip_level).r;
            let depth_2 = params.prefiltered_depth.SampleLevel(params.linear_clamp_sampler, sample_screen_pos_2, sample_mip_level).r;

            let sample_position_1 = reconstruct_view_space_position(sample_screen_pos_1, depth_1, params.camera);
            let sample_position_2 = reconstruct_view_space_position(sample_screen_pos_2, depth_2, params.camera);

            let sample_delta_1 = sample_position_1 - origin;
            let sample_delta_2 = sample_position_2 - origin;
            bitmask = calc_visibility_mask(bitmask, thickness, sample_delta_1, view_dir,  1.0, n);
            bitmask = calc_visibility_mask(bitmask, thickness, sample_delta_2, view_dir, -1.0, n);
        }

        visibility += 1.0 - f32(countbits(bitmask)) / f32(SECTOR_COUNT);
    }

    let ao = saturate(visibility / slice_count);
    params.ambient_occlusion[pixel_coordinates] = ao;
}