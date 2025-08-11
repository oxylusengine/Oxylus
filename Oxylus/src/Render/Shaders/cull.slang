module cull;

import common;
import scene;
import gpu;

public func normalize_plane(f32x4 p) -> f32x4 {
    return p / length(p.xyz);
}

public struct ScreenAabb {
    public f32x3 min;
    public f32x3 max;
}

// https://zeux.io/2023/01/12/approximate-projected-bounds
// returns the AABB in UV space!!! not clip space
public func project_aabb(f32x4x4 mvp, f32 near, f32x3 aabb_min, f32x3 aabb_extent) -> Optional<ScreenAabb> {
    let SX = mul(mvp, f32x4(aabb_extent.x, 0.0, 0.0, 0.0));
    let SY = mul(mvp, f32x4(0.0, aabb_extent.y, 0.0, 0.0));
    let SZ = mul(mvp, f32x4(0.0, 0.0, aabb_extent.z, 0.0));

    let P0 = mul(mvp, f32x4(aabb_min, 1.0));
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
    //vaabb = vaabb.xwzy * f32x4(0.5, -0.5, 0.5, -0.5) + 0.5;
    vaabb = vaabb * 0.5 + 0.5;
    ScreenAabb ret = { f32x3(vaabb.xy, vmin.z), f32x3(vaabb.zw, vmax.z) };
    return ret;
}

func project_sphere(f32x3 center, f32 radius, f32 near, f32 P00, f32 P11) -> Optional<ScreenAabb> {
    if (center.z < radius + near) {
        return none;
    }

    let cr = center * radius;
    let czr2 = center.z * center.z - radius * radius;

    let vx = sqrt(center.x * center.x + czr2);
    let minx = (vx * center.x - cr.z) / (vx * center.z + cr.x);
    let maxx = (vx * center.x + cr.z) / (vx * center.z - cr.x);

    let vy = sqrt(center.y * center.y + czr2);
    let miny = (vy * center.y - cr.z) / (vy * center.z + cr.y);
    let maxy = (vy * center.y + cr.z) / (vy * center.z - cr.y);

    var vaabb = f32x4(minx * P00, miny * P11, maxx * P00, maxy * P11);
    //vaabb = vaabb.xwzy * f32x4(0.5f, -0.5f, 0.5f, -0.5f) + f32x4(0.5f); // clip space -> uv space
    vaabb = vaabb * 0.5 + 0.5;
    ScreenAabb ret = { f32x3(vaabb.xy, 0.0), f32x3(vaabb.zw, 0.0) };
    return ret;
}

public func test_occlusion(
    in Image2D<f32> hiz_image,
    in Sampler hiz_sampler,
    in ScreenAabb screen_aabb
) -> bool {
    var hiz_size = u32x2(0.0);
    var hiz_levels = 0;
    // TODO: Replace this with push constants
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
