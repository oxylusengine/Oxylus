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

public func vec3_to_oct(f32x3 v) -> f32x2 {
    const let p = v.xy * (1.0 / (abs(v.x) + abs(v.y) + abs(v.z)));
    const let sign_not_zero = f32x2((p.x >= 0.0) ? 1.0 : -1.0, (p.y >= 0.0) ? 1.0 : -1.0);
    return (v.z <= 0.0) ? ((1.0 - abs(p.yx)) * sign_not_zero) : p;
}

}