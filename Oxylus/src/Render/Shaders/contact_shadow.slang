import common;
import gpu;

public interface IContactDepth {
    f32 load(int2 pixel, int2 extent);
};

public struct ScreenContactDepth : IContactDepth {
    public Texture2D<f32> image;

    public f32 load(int2 pixel, int2 extent) {
        return all(pixel >= 0) && all(pixel < extent) ? image.Load(int3(pixel, 0)) : 0.0;
    }
};

// depth difference along view Z, including orthographic and finite reversed-Z projections
f32 depth_gap(f32 a, f32 b, f32x3 unpack) {
    let inv_w = max(f32x2(a, b) * unpack.x + unpack.y, 1e-20);
    return (a - b) * unpack.z / (inv_w.x * inv_w.y);
}

// depth is affine across a triangle. at a crease, choose the pair whose slope
// extrapolates back to the receiver, even if the other face happens to be closer
public f32 receiver_slope<T : IContactDepth>(T cache, int2 pixel, int2 axis, int2 extent, f32 center) {
    let before = cache.load(pixel - axis, extent);
    let after = cache.load(pixel + axis, extent);
    let left = center - before;
    let right = after - center;
    if (before <= 0.0) return after > 0.0 ? right : 0.0;
    if (after <= 0.0) return left;
    let before2 = cache.load(pixel - 2 * axis, extent);
    let after2 = cache.load(pixel + 2 * axis, extent);
    let left_error = before2 > 0.0 ? abs(left - (before - before2)) : abs(left);
    let right_error = after2 > 0.0 ? abs(right - (after2 - after)) : abs(right);
    return left_error < right_error ? left : right;
}

f32 blocker(f32 gap, f32 thickness) {
    let ratio = gap / thickness;
    return smoothstep(0.01, 0.08, ratio) * (1.0 - smoothstep(0.7, 1.0, ratio));
}

f32 tap_coverage(f32 a, f32 b, f32 fraction, f32 ray_depth, f32 minor_slope, ContactShadowTraceSettings settings) {
    let unpack = settings.depth_unpack;
    let thickness = settings.thickness;
    if (a > 0.0 && b > 0.0 && abs(depth_gap(a, b, unpack)) < thickness) {
        // interpolate device depth because it is affine over a rasterized triangle
        return blocker(depth_gap(lerp(a, b, fraction), ray_depth, unpack), thickness);
    }

    // at a silhouette, blend the coverage of the two real surfaces. compare at
    // their texel centers so steep receivers stay lit even at grazing angles
    let depth_a = ray_depth - fraction * minor_slope;
    let depth_b = ray_depth + (1.0 - fraction) * minor_slope;
    let hit_a = a > 0.0 ? blocker(depth_gap(a, depth_a, unpack), thickness) : 0.0;
    let hit_b = b > 0.0 ? blocker(depth_gap(b, depth_b, unpack), thickness) : 0.0;
    return lerp(hit_a, hit_b, fraction);
}

public struct ContactRay {
    public f32x2 pixel_step;
    public f32 depth_step;
    public f32 reach;
    public uint steps;
    public bool vertical;
    public int major_sign;
};

public ContactRay project_contact_ray(
    int2 pixel, int2 extent, f32 depth, f32x4 light, f32 length_ws, ContactShadowTraceSettings settings
) {
    let half_extent = 0.5 * f32x2(extent);

    // for clip(t) = origin + t * light, the projected line has constant direction
    // computing it before the perspective divide also handles a light direction parallel to the eye plane
    let delta = half_extent * (light.xy + light.w) - (f32x2(pixel) + 0.5) * light.w;
    let major = max(abs(delta.x), abs(delta.y));
    let unpack = settings.depth_unpack;
    let start_w = rcp(max(depth * unpack.x + unpack.y, 1e-20));
    let end_w = start_w + length_ws * light.w;
    let budget = min(settings.steps, CONTACT_SHADOW_MAX_STEPS);
    let reach = min(f32(budget), major * length_ws / max(end_w, 1e-10));
    let steps = depth > 0.0 && major > 1e-6 && settings.thickness > 0.0 ? uint(reach) : 0u;

    let pixel_step = delta / max(major, 1e-6);
    let vertical = abs(delta.y) > abs(delta.x);
    return {pixel_step, (light.z - depth * light.w) / max(major, 1e-6), reach, steps,
            vertical, (vertical ? delta.y : delta.x) < 0.0 ? -1 : 1};
}

public interface IContactLine {
    f32x2 load_pair(uint step, int row);
};

public struct ScreenContactLine : IContactLine {
    public ScreenContactDepth source;
    public int2 pixel;
    public int2 extent;
    public bool vertical;
    public int major_sign;

    public f32x2 load_pair(uint step, int row) {
        let offset = int2(major_sign * int(step), row);
        let p = pixel + (vertical ? offset.yx : offset);
        return f32x2(source.load(p, extent), source.load(p + (vertical ? int2(1, 0) : int2(0, 1)), extent));
    }
};

public f32 trace_contact_ray<T : IContactLine>(
    T line, ContactRay ray, int start_row, f32 depth, f32x2 slope, ContactShadowTraceSettings settings
) {
    if (ray.depth_step < dot(slope, ray.pixel_step)) return 1.0;
    let minor_step = ray.vertical ? ray.pixel_step.x : ray.pixel_step.y;
    let minor_slope = ray.vertical ? slope.x : slope.y;
    var contact = 0.0;
    var distant = f32x4(0.0);
    for (uint i = 1; i <= ray.steps; ++i) {
        let minor = f32(start_row) + minor_step * f32(i);
        let ray_depth = depth + ray.depth_step * f32(i);
        if (ray_depth <= 0.0 || ray_depth > 1.0) break;
        let taps = line.load_pair(i, int(floor(minor)));
        var hit = tap_coverage(taps.x, taps.y, frac(minor), ray_depth, minor_slope, settings);
        hit *= 1.0 - smoothstep(0.75, 1.0, f32(i) / (ray.reach + 0.5));

        // Bend's hard contact taps and four interleaved sets suppress streaks from
        // isolated depth samples while retaining fine detail next to the caster
        contact = max(contact, hit * (1.0 - smoothstep(3.0, 8.0, f32(i))));
        distant[i & 3] = max(distant[i & 3], hit);
        if (contact >= 1.0) break;
    }
    return 1.0 - max(contact, dot(distant, 0.25));
}
