import common;
import gpu;
import scene;

struct ShaderParameters {
    Texture2D<f32> occlusion_noisy;
    Texture2D<u32> depth_differences;
    RWTexture2D<f32> ambient_occlusion;
    Sampler point_clamp_sampler;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x2 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform i32x3 occlusion_noisy_extent
) -> void {
    let pixel_coordinates = i32x2(thread_id.xy);
    let uv = f32x2(pixel_coordinates) / f32x2(occlusion_noisy_extent.xy);

    let edges0 = params.depth_differences.GatherRed(params.point_clamp_sampler, uv);
    let edges1 = params.depth_differences.GatherRed(params.point_clamp_sampler, uv, i32x2(2, 0));
    let edges2 = params.depth_differences.GatherRed(params.point_clamp_sampler, uv, i32x2(1, 2));
    let visibility0 = params.occlusion_noisy.GatherRed(params.point_clamp_sampler, uv);
    let visibility1 = params.occlusion_noisy.GatherRed(params.point_clamp_sampler, uv, i32x2(2, 0));
    let visibility2 = params.occlusion_noisy.GatherRed(params.point_clamp_sampler, uv, i32x2(0, 2));
    let visibility3 = params.occlusion_noisy.GatherRed(params.point_clamp_sampler, uv, i32x2(2, 2));

    let left_edges = unpackUnorm4x8ToFloat(edges0.x);
    let right_edges = unpackUnorm4x8ToFloat(edges1.x);
    let top_edges = unpackUnorm4x8ToFloat(edges0.z);
    let bottom_edges = unpackUnorm4x8ToFloat(edges2.w);
    var center_edges = unpackUnorm4x8ToFloat(edges0.y);
    center_edges *= f32x4(left_edges.y, right_edges.x, top_edges.w, bottom_edges.z);

    let center_weight = 1.2;
    let left_weight = center_edges.x;
    let right_weight = center_edges.y;
    let top_weight = center_edges.z;
    let bottom_weight = center_edges.w;
    let top_left_weight = 0.425 * (top_weight * top_edges.x + left_weight * left_edges.z);
    let top_right_weight = 0.425 * (top_weight * top_edges.y + right_weight * right_edges.z);
    let bottom_left_weight = 0.425 * (bottom_weight * bottom_edges.x + left_weight * left_edges.w);
    let bottom_right_weight = 0.425 * (bottom_weight * bottom_edges.y + right_weight * right_edges.w);

    let center_visibility = visibility0.y;
    let left_visibility = visibility0.x;
    let right_visibility = visibility0.z;
    let top_visibility = visibility1.x;
    let bottom_visibility = visibility2.z;
    let top_left_visibility = visibility0.w;
    let top_right_visibility = visibility1.w;
    let bottom_left_visibility = visibility2.w;
    let bottom_right_visibility = visibility3.w;

    var sum = center_visibility;
    sum += left_visibility * left_weight;
    sum += right_visibility * right_weight;
    sum += top_visibility * top_weight;
    sum += bottom_visibility * bottom_weight;
    sum += top_left_visibility * top_left_weight;
    sum += top_right_visibility * top_right_weight;
    sum += bottom_left_visibility * bottom_left_weight;
    sum += bottom_right_visibility * bottom_right_weight;

    var sum_weight = center_weight;
    sum_weight += left_weight;
    sum_weight += right_weight;
    sum_weight += top_weight;
    sum_weight += bottom_weight;
    sum_weight += top_left_weight;
    sum_weight += top_right_weight;
    sum_weight += bottom_left_weight;
    sum_weight += bottom_right_weight;

    let denoised_visibility = sum / sum_weight;

    params.ambient_occlusion.Store(pixel_coordinates, denoised_visibility);
}