module cull;

import common;
import scene;

public func normalize_plane(f32x4 p) -> f32x4 {
    return p / length(p.xyz);
}

public struct ScreenAabb {
    public f32x3 min;
    public f32x3 max;
}

// https://zeux.io/2023/01/12/approximate-projected-bounds
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

    if (max(max(max(max(max(max(max(P0.z, P1.z), P2.z), P3.z), P4.z), P5.z), P6.z), P7.z) < near) {
        return none;
    }

    let vmin = min(
        min(min(P0.xyz / P0.w, P1.xyz / P1.w), min(P2.xyz / P2.w, P3.xyz / P3.w)),
        min(min(P4.xyz / P4.w, P5.xyz / P5.w), min(P6.xyz / P6.w, P7.xyz / P7.w))
    );
    let vmax = max(
        max(max(P0.xyz / P0.w, P1.xyz / P1.w), max(P2.xyz / P2.w, P3.xyz / P3.w)),
        max(max(P4.xyz / P4.w, P5.xyz / P5.w), max(P6.xyz / P6.w, P7.xyz / P7.w))
    );

    var vaabb = f32x4(vmin.xy, vmax.xy);
    vaabb = vaabb.xwzy * f32x4(0.5, -0.5, 0.5, -0.5) + 0.5;

    ScreenAabb ret = { f32x3(vaabb.xy, vmin.z), f32x3(vaabb.zw, vmax.z) };
    return ret;
}

