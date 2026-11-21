implementing common;

public namespace com {
public func hash_u32x3(u32x3 v) -> u32 {
    var h = v.x * 73856093u ^ v.y * 19349663u ^ v.z * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// gradient noise roughly in [-1, 1], the lattice repeats every `period` cells, which must be a power of two
public func periodic_gradient_noise(f32x3 p, u32 period) -> f32 {
    let cell = i32x3(floor(p));
    let f = p - floor(p);
    let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    var result = 0.0;
    [ForceUnroll]
    for (u32 corner = 0; corner < 8; corner++) {
        let offset = i32x3(i32(corner & 1u), i32((corner >> 1) & 1u), i32((corner >> 2) & 1u));
        let h = hash_u32x3(u32x3(cell + offset) & (period - 1u));
        let gradient = f32x3(f32(h & 1023u), f32((h >> 10) & 1023u), f32((h >> 20) & 1023u)) / 511.5 - 1.0;
        let ramp = dot(gradient, f - f32x3(offset));
        let weight = select(offset.x != 0, u.x, 1.0 - u.x) * select(offset.y != 0, u.y, 1.0 - u.y) *
                     select(offset.z != 0, u.z, 1.0 - u.z);
        result += ramp * weight;
    }

    return result * 1.6;
}
}
