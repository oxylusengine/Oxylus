module cull;

import common;
import gpu;
import debug_drawer;

public struct ScreenAabb {
    public f32x3 min;
    public f32x3 max;
}

// Credits:
// - https://zeux.io/2023/01/12/approximate-projected-bounds
public func project_aabb(f32x4x4 mvp, f32 near, f32x3 aabb_center, f32x3 aabb_extent) -> Optional<ScreenAabb> {
    let SX = mul(mvp, f32x4(aabb_extent.x, 0.0, 0.0, 0.0));
    let SY = mul(mvp, f32x4(0.0, aabb_extent.y, 0.0, 0.0));
    let SZ = mul(mvp, f32x4(0.0, 0.0, aabb_extent.z, 0.0));

    let P0 = mul(mvp, f32x4(aabb_center - aabb_extent * 0.5, 1.0));
    let P1 = P0 + SZ;
    let P2 = P0 + SY;
    let P3 = P2 + SZ;
    let P4 = P0 + SX;
    let P5 = P4 + SZ;
    let P6 = P4 + SY;
    let P7 = P6 + SZ;

    let depth = min(P0, min(P1, min(P2, min(P3, min(P4, min(P5, min(P6, P7))))))).w;
    if (depth < near) {
        return none;
    }

    let DP0 = P0.xyz / P0.w;
    let DP1 = P1.xyz / P1.w;
    let DP2 = P2.xyz / P2.w;
    let DP3 = P3.xyz / P3.w;
    let DP4 = P4.xyz / P4.w;
    let DP5 = P5.xyz / P5.w;
    let DP6 = P6.xyz / P6.w;
    let DP7 = P7.xyz / P7.w;
    let vmin = min(DP0, min(DP1, min(DP2, min(DP3, min(DP4, min(DP5, min(DP6, DP7)))))));
    let vmax = max(DP0, max(DP1, max(DP2, max(DP3, max(DP4, max(DP5, max(DP6, DP7)))))));

    var vaabb = f32x4(vmin.xy, vmax.xy);
    // clip to uv space
    vaabb = vaabb * 0.5 + 0.5;
    ScreenAabb ret = { f32x3(vaabb.xy, vmin.z), f32x3(vaabb.zw, vmax.z) };
    return ret;
}

func normalize_plane(f32x4 p) -> f32x4 {
    return p / length(p.xyz);
}

// Credits:
// - https://github.com/SparkyPotato/radiance/blob/eaf18b3bbf4942234fa929ef6ad5e04e3c45fc62/shaders/passes/mesh/cull.slang#L340
// - https://fgiesen.wordpress.com/2012/08/31/frustum-planes-from-the-projection-matrix/
// - https://fgiesen.wordpress.com/2010/10/17/view-frustum-culling/
public func test_frustum(in f32x4x4 mvp, in f32x3 aabb_center, in f32x3 aabb_extent) -> bool {
    f32x4 planes[] = {
        // Left
        normalize_plane(mvp[3] + mvp[0]),
        // Right
        normalize_plane(mvp[3] - mvp[0]),
        // Bottom
        normalize_plane(mvp[3] + mvp[1]),
        // Top
        normalize_plane(mvp[3] - mvp[1]),
        // Near
        normalize_plane(mvp[2]),
        // Far
        normalize_plane(mvp[3] - mvp[2])
    };

    let aabb_half_extent = aabb_extent * 0.5;
    [[unroll]]
    for (uint i = 0; i < planes.getCount(); i++) {
        let flip = asuint(planes[i].xyz) & 0x80000000;
        let sign_flipped = asfloat(asuint(aabb_half_extent) ^ flip);
        if (dot(aabb_center + sign_flipped, planes[i].xyz) <= -planes[i].w) {
            return false;
        }
    }

    return true;
}

public func test_occlusion(
    in ScreenAabb screen_aabb,
    in Image2D<f32> hiz_image,
    in Sampler hiz_sampler
) -> bool {
    var hiz_size = u32x2(0.0);
    var hiz_levels = 0;
    hiz_image.GetDimensions(0, hiz_size.x, hiz_size.y, hiz_levels);

    let min_uv = screen_aabb.min.xy;
    let max_uv = screen_aabb.max.xy;
    let min_texel = u32x2(clamp(min_uv * f32x2(hiz_size), 0.0, hiz_size - 1.0));
    let max_texel = u32x2(clamp(max_uv * f32x2(hiz_size), 0.0, hiz_size - 1.0));

    let size = max_texel - min_texel + 1;
    let max_size = max(size.x, size.y);
    var mip = firstbithigh(max_size - 1) - 1;
    let smin = min_texel >> mip;
    let smax = max_texel >> mip;
    if (any(smax - smin > 1)) {
        mip += 1;
    }

    var uv = (min_uv + max_uv) * 0.5;
    let d = hiz_image.sample_mip(hiz_sampler, uv, mip);
    return screen_aabb.max.z <= d;
}

