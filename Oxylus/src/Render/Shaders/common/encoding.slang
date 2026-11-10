implementing common;

public namespace com {
// Unit vectors octahedral encoding
// "Survey of Efficient Representations for Independent Unit Vectors"
// http://jcgt.org/published/0003/02/01
public func oct_to_vec3(f32x2 e) -> f32x3 {
    f32x3 v = f32x3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    const let sign_not_zero = f32x2((v.x >= 0.0) ? +1.0 : -1.0, (v.y >= 0.0) ? +1.0 : -1.0);
    if (v.z < 0.0) {
        v.xy = (1.0 - abs(v.yx)) * sign_not_zero;
    }

    return normalize(v);
}

public func pack_snorm_2x(f32x2 value, f32 scale, u32 bits) -> u32 {
    let d = u32x2(round(scale + value * scale));
    return d.x | (d.y << bits);
}

public func unpack_snorm_2x(u32 packed, f32 scale, u32 bits) -> f32x2 {
    let mask = (1u << bits) - 1u;
    return f32x2(u32x2(packed, packed >> bits) & mask) / scale - 1.0;
}

public func compress_normal_24(f32x3 normal) -> u32 {
    return pack_snorm_2x(vec3_to_oct(normal), 2047.5, 12u);
}

public func decompress_normal_24(u32 packed) -> f32x3 {
    return oct_to_vec3(unpack_snorm_2x(packed, 2047.5, 12u));
}

public func compress_normal_32(f32x3 normal) -> u32 {
    return pack_snorm_2x(vec3_to_oct(normal), 32767.5, 16u);
}

public func decompress_normal_32(u32 packed) -> f32x3 {
    return oct_to_vec3(unpack_snorm_2x(packed, 32767.5, 16u));
}

public func vec3_to_oct(f32x3 v) -> f32x2 {
    const let p = v.xy * (1.0 / (abs(v.x) + abs(v.y) + abs(v.z)));
    const let sign_not_zero = f32x2((p.x >= 0.0) ? 1.0 : -1.0, (p.y >= 0.0) ? 1.0 : -1.0);
    return (v.z <= 0.0) ? ((1.0 - abs(p.yx)) * sign_not_zero) : p;
}

}