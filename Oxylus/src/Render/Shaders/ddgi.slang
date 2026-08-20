import common;

public constexpr static u32 DEBUG_SPHERE_RINGS = 8;
public constexpr static u32 DEBUG_SPHERE_SECTORS = 12;
public constexpr static u32 DEBUG_SPHERE_VERTEX_COUNT = DEBUG_SPHERE_RINGS * DEBUG_SPHERE_SECTORS * 6;

public struct ProbeVolume {
    public f32x3 origin;
    public u32 probe_offset;
    public f32x3 spacing;
    public u32 _pad_1;
    public u32x3 counts;
    public u32 probe_count;

    public func extents() -> f32x3 {
        return f32x3(max(this.counts, 1u) - 1u) * this.spacing * 0.5;
    }

    public func probe_coord(u32 probe_index) -> u32x3 {
        let plane = this.counts.x * this.counts.y;
        return u32x3(probe_index % this.counts.x, (probe_index % plane) / this.counts.x, probe_index / plane);
    }

    public func probe_position(u32 probe_index) -> f32x3 {
        return this.origin - this.extents() + f32x3(this.probe_coord(probe_index)) * this.spacing;
    }
};

func pcg_hash(u32 v) -> u32 {
    let state = v * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

func next_f32(inout u32 seed) -> f32 {
    seed = pcg_hash(seed);
    return f32(seed) * 2.3283064365386963e-10;
}

func rotate_by_quat(f32x4 q, f32x3 v) -> f32x3 {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

func frame_rotation(u32 frame_index) -> f32x4 {
    var seed = pcg_hash(frame_index);
    let u1 = next_f32(seed);
    let u2 = next_f32(seed);
    let u3 = next_f32(seed);

    let r1 = sqrt(1.0 - u1);
    let r2 = sqrt(u1);

    return f32x4(r1 * sin(TAU * u2), r1 * cos(TAU * u2), r2 * sin(TAU * u3), r2 * cos(TAU * u3));
}

func spherical_fibonacci(u32 index, u32 count) -> f32x3 {
    const f32 GOLDEN_ANGLE = PI * (3.0 - sqrt(5.0));

    let z = 1.0 - (2.0 * f32(index) + 1.0) / f32(count);
    let r = sqrt(saturate(1.0 - z * z));
    let phi = f32(index) * GOLDEN_ANGLE;

    return f32x3(cos(phi) * r, sin(phi) * r, z);
}

public func probe_ray_direction(u32 ray_index, u32 ray_count, u32 frame_index) -> f32x3 {
    return normalize(rotate_by_quat(frame_rotation(frame_index), spherical_fibonacci(ray_index, ray_count)));
}

public func debug_sphere_vertex(u32 vertex_index) -> f32x3 {
    const u32x2 corners[6] = {
        u32x2(0, 0), u32x2(1, 0), u32x2(1, 1),
        u32x2(0, 0), u32x2(1, 1), u32x2(0, 1),
    };

    let quad_index = vertex_index / 6;
    let corner = corners[vertex_index % 6];
    let sector = quad_index % DEBUG_SPHERE_SECTORS + corner.x;
    let ring = quad_index / DEBUG_SPHERE_SECTORS + corner.y;

    let phi = f32(sector) / f32(DEBUG_SPHERE_SECTORS) * TAU;
    let theta = f32(ring) / f32(DEBUG_SPHERE_RINGS) * PI;

    return f32x3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}
