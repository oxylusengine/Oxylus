import common;
import gpu;
import scene;

struct ShaderParameters {
    Texture2D<f32> src_depth;
    RWTexture2D<f32> dst_depth_mip0;
    RWTexture2D<f32> dst_depth_mip1;
    RWTexture2D<f32> dst_depth_mip2;
    RWTexture2D<f32> dst_depth_mip3;
    RWTexture2D<f32> dst_depth_mip4;
    Sampler point_clamp_sampler;
};

groupshared float previous_mip_depth[8][8];

float weighted_average(float depth0, float depth1, float depth2, float depth3) {
    float depth_range_scale_factor = 0.75;
    float effect_radius = depth_range_scale_factor * 0.5 * 1.457;
    float falloff_range = 0.615 * effect_radius;
    float falloff_from = effect_radius * (1.0 - 0.615);
    float falloff_mul = -1.0 / falloff_range;
    float falloff_add = falloff_from / falloff_range + 1.0;

    float min_depth = min(min(depth0, depth1), min(depth2, depth3));
    float weight0 = saturate((depth0 - min_depth) * falloff_mul + falloff_add);
    float weight1 = saturate((depth1 - min_depth) * falloff_mul + falloff_add);
    float weight2 = saturate((depth2 - min_depth) * falloff_mul + falloff_add);
    float weight3 = saturate((depth3 - min_depth) * falloff_mul + falloff_add);
    float weight_total = weight0 + weight1 + weight2 + weight3;

    return ((weight0 * depth0) + (weight1 * depth1) + (weight2 * depth2) + (weight3 * depth3)) / weight_total;
}

[[numthreads(8, 8, 1)]]
func cs_main(
    uint3 global_id : SV_DispatchThreadID,
    uint3 local_id  : SV_GroupThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform i32x3 depth_extent
) -> void {
    int2 base_coordinates = int2(global_id.xy);

    // MIP 0
    int2 pixel_coordinates0 = base_coordinates * 2;
    int2 pixel_coordinates1 = pixel_coordinates0 + int2(1, 0);
    int2 pixel_coordinates2 = pixel_coordinates0 + int2(0, 1);
    int2 pixel_coordinates3 = pixel_coordinates0 + int2(1, 1);

    float2 depths_uv = float2(pixel_coordinates0) / f32x2(depth_extent.xy);
    float4 depths = params.src_depth.Gather(params.point_clamp_sampler, depths_uv, int2(1,1));

    params.dst_depth_mip0[pixel_coordinates0] = depths.w;
    params.dst_depth_mip0[pixel_coordinates1] = depths.z;
    params.dst_depth_mip0[pixel_coordinates2] = depths.x;
    params.dst_depth_mip0[pixel_coordinates3] = depths.y;

    // MIP 1
    float depth_mip1 = weighted_average(depths.w, depths.z, depths.x, depths.y);
    params.dst_depth_mip1[base_coordinates] = depth_mip1;
    previous_mip_depth[local_id.x][local_id.y] = depth_mip1;

    GroupMemoryBarrierWithGroupSync();

    // MIP 2
    if ((local_id.x % 2 == 0) && (local_id.y % 2 == 0)) {
        float d0 = previous_mip_depth[local_id.x + 0][local_id.y + 0];
        float d1 = previous_mip_depth[local_id.x + 1][local_id.y + 0];
        float d2 = previous_mip_depth[local_id.x + 0][local_id.y + 1];
        float d3 = previous_mip_depth[local_id.x + 1][local_id.y + 1];
        float depth_mip2 = weighted_average(d0, d1, d2, d3);
        params.dst_depth_mip2[base_coordinates / 2] = depth_mip2;
        previous_mip_depth[local_id.x][local_id.y] = depth_mip2;
    }

    GroupMemoryBarrierWithGroupSync();

    // MIP 3
    if ((local_id.x % 4 == 0) && (local_id.y % 4 == 0)) {
        float d0 = previous_mip_depth[local_id.x + 0][local_id.y + 0];
        float d1 = previous_mip_depth[local_id.x + 2][local_id.y + 0];
        float d2 = previous_mip_depth[local_id.x + 0][local_id.y + 2];
        float d3 = previous_mip_depth[local_id.x + 2][local_id.y + 2];
        float depth_mip3 = weighted_average(d0, d1, d2, d3);
        params.dst_depth_mip3[base_coordinates / 4] = depth_mip3;
        previous_mip_depth[local_id.x][local_id.y] = depth_mip3;
    }

    GroupMemoryBarrierWithGroupSync();

    // MIP 4
    if ((local_id.x % 8 == 0) && (local_id.y % 8 == 0)) {
        float d0 = previous_mip_depth[local_id.x + 0][local_id.y + 0];
        float d1 = previous_mip_depth[local_id.x + 4][local_id.y + 0];
        float d2 = previous_mip_depth[local_id.x + 0][local_id.y + 4];
        float d3 = previous_mip_depth[local_id.x + 4][local_id.y + 4];
        float depth_mip4 = weighted_average(d0, d1, d2, d3);
        params.dst_depth_mip4[base_coordinates / 8] = depth_mip4;
    }
}