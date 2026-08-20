import common;
import gpu;

public constexpr static u32 IRRADIANCE_TEXELS = 6;
public constexpr static u32 DISTANCE_TEXELS = 14;
public constexpr static u32 PROBES_PER_ATLAS_ROW = 32;
public constexpr static f32 DISTANCE_SHARPNESS = 50.0;

public constexpr static u32 DEBUG_SPHERE_RINGS = 8;
public constexpr static u32 DEBUG_SPHERE_SECTORS = 12;
public constexpr static u32 DEBUG_SPHERE_VERTEX_COUNT = DEBUG_SPHERE_RINGS * DEBUG_SPHERE_SECTORS * 6;

public static const u32 PROBE_INACTIVE_BIT = 1u;
public static const u32 PROBE_UPDATE_BIT = 2u;
public static const u32 PROBE_RESET_BIT = 4u;

// Above this share of backface hits a probe is taken to be inside geometry rather than merely facing a
// wall. Coming back out is held to the lower figure: the ray set is rotated every frame, so a probe
// sitting near the threshold would otherwise cross it on ray noise alone and blink in and out of the
// eight probe blend.
public static const f32 PROBE_INSIDE_BACKFACE_RATIO = 0.25;
public static const f32 PROBE_ESCAPED_BACKFACE_RATIO = 0.1;

public struct ProbeState {
    public f32x3 offset;
    public u32 flags;
};

public func probe_is_inactive(u32 flags) -> bool {
    return (flags & PROBE_INACTIVE_BIT) != 0;
}

public func probe_updates_this_frame(u32 flags) -> bool {
    return (flags & PROBE_UPDATE_BIT) != 0;
}

public func probe_is_reset(u32 flags) -> bool {
    return (flags & PROBE_RESET_BIT) != 0;
}

public func probe_update_interval(u32 flags) -> u32 {
    return max((flags >> 8) & 0xffu, 1u);
}

public func probe_pack_flags(bool inactive, bool update, bool reset, u32 interval) -> u32 {
    return (inactive ? PROBE_INACTIVE_BIT : 0u) | (update ? PROBE_UPDATE_BIT : 0u) |
           (reset ? PROBE_RESET_BIT : 0u) | (min(interval, 0xffu) << 8);
}

public struct ProbeVolume {
    public f32x3 origin;
    public u32 probe_offset;
    public f32x3 spacing;
    public u32 _pad_1;
    public u32x3 counts;
    public u32 probe_count;
    public i32x3 scroll;
    public u32 camera_locked;
    public f32x3 blend_origin;
    public u32 _pad_2;

    public func extents() -> f32x3 {
        return f32x3(max(this.counts, 1u) - 1u) * this.spacing * 0.5;
    }

    func wrap(i32x3 coord) -> u32x3 {
        let n = i32x3(this.counts);
        return u32x3(((coord % n) + n) % n);
    }

    public func probe_coord(u32 probe_index) -> u32x3 {
        let plane = this.counts.x * this.counts.y;
        return u32x3(probe_index % this.counts.x, (probe_index % plane) / this.counts.x, probe_index / plane);
    }

    public func grid_coord(u32 probe_index) -> u32x3 {
        return this.wrap(i32x3(this.probe_coord(probe_index)) - this.scroll);
    }

    public func probe_position(u32 probe_index) -> f32x3 {
        return this.grid_min() + f32x3(this.grid_coord(probe_index)) * this.spacing;
    }

    public func grid_min() -> f32x3 {
        return this.origin - this.extents();
    }

    public func probe_index_at(u32x3 grid_coordinate) -> u32 {
        let storage = this.wrap(i32x3(grid_coordinate) + this.scroll);
        return storage.x + storage.y * this.counts.x + storage.z * this.counts.x * this.counts.y;
    }

    public func max_probe_distance() -> f32 {
        return length(this.spacing) * 1.5;
    }

    public func blend_weight(f32x3 world_position) -> f32 {
        let tightening = this.camera_locked != 0 ? this.spacing : f32x3(0.0);
        let reach = max(this.extents() - tightening, f32x3(0.0));
        let delta = abs(world_position - this.blend_origin) - reach;
        let fade = saturate(1.0 - delta / this.spacing);
        return fade.x * fade.y * fade.z;
    }
};

public func probe_texel_direction(u32x2 texel, u32 interior_texels) -> f32x3 {
    return com::oct_to_vec3((f32x2(texel) + 0.5) / f32(interior_texels) * 2.0 - 1.0);
}

public func probe_tile_origin(u32 probe_row, u32 interior_texels) -> u32x2 {
    let tile = interior_texels + 2;
    return u32x2(probe_row % PROBES_PER_ATLAS_ROW, probe_row / PROBES_PER_ATLAS_ROW) * tile;
}

public func probe_atlas_uv(u32 probe_row, f32x3 direction, u32 interior_texels, f32x2 atlas_size) -> f32x2 {
    let origin = f32x2(probe_tile_origin(probe_row, interior_texels));
    let octant = com::vec3_to_oct(normalize(direction)) * 0.5 + 0.5;

    return (origin + 1.0 + octant * f32(interior_texels)) / atlas_size;
}

public func sample_probe_irradiance(Image2D<f32x4> atlas, Sampler atlas_sampler, u32 probe_row, f32x3 direction)
    -> f32x3 {
    u32 width, height;
    atlas.GetDimensions(width, height);

    let uv = probe_atlas_uv(probe_row, direction, IRRADIANCE_TEXELS, f32x2(width, height));
    return atlas.SampleLevel(atlas_sampler, uv, 0.0).rgb;
}

public func sample_probe_distance(Image2D<f32x2> atlas, Sampler atlas_sampler, u32 probe_row, f32x3 direction)
    -> f32x2 {
    u32 width, height;
    atlas.GetDimensions(width, height);

    let uv = probe_atlas_uv(probe_row, direction, DISTANCE_TEXELS, f32x2(width, height));
    return atlas.SampleLevel(atlas_sampler, uv, 0.0).rg;
}

public func probe_border_source(u32x2 texel, u32 interior_texels) -> u32x2 {
    let last = interior_texels + 1;
    let on_x_border = texel.x == 0 || texel.x == last;
    let on_y_border = texel.y == 0 || texel.y == last;

    if (on_x_border && on_y_border) {
        return u32x2(texel.x == 0 ? interior_texels : 1, texel.y == 0 ? interior_texels : 1);
    }

    if (on_x_border) {
        return u32x2(texel.x == 0 ? 1 : interior_texels, last - texel.y);
    }

    return u32x2(last - texel.x, texel.y == 0 ? 1 : interior_texels);
}

public func probe_border_texel(u32 index, u32 interior_texels) -> u32x2 {
    let tile = interior_texels + 2;
    if (index < tile) {
        return u32x2(index, 0);
    }
    if (index < tile * 2) {
        return u32x2(index - tile, tile - 1);
    }
    if (index < tile * 3 - 2) {
        return u32x2(0, index - tile * 2 + 1);
    }

    return u32x2(tile - 1, index - (tile * 3 - 2) + 1);
}

public func probe_border_texel_count(u32 interior_texels) -> u32 {
    return (interior_texels + 2) * 4 - 4;
}

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

public func probe_world_position(in ProbeVolume volume, StructuredBuffer<ProbeState> states, u32 probe_index)
    -> f32x3 {
    return volume.probe_position(probe_index) + states[volume.probe_offset + probe_index].offset;
}

static const f32 MIN_TOTAL_WEIGHT = 1e-4;

public struct IrradianceSample {
    public f32x3 irradiance;
    public f32x3 specular;
    public f32 coverage;
};

public func sample_probe_volume(
    in ProbeVolume volume,
    StructuredBuffer<ProbeState> states,
    Image2D<f32x4> irradiance_atlas,
    Image2D<f32x2> distance_atlas,
    Sampler atlas_sampler,
    f32x3 world_position,
    f32x3 normal,
    f32x3 view_direction,
    f32x3 specular_direction,
    bool want_specular,
    f32 normal_bias,
    f32 view_bias
) -> IrradianceSample {
    IrradianceSample result = { f32x3(0.0), f32x3(0.0), 0.0 };

    let cell = min(min(volume.spacing.x, volume.spacing.y), volume.spacing.z);
    let biased_position = world_position + (normal * normal_bias + view_direction * view_bias) * cell;
    let grid_min = volume.grid_min();
    let grid_position = (biased_position - grid_min) / volume.spacing;
    let base_coord = clamp(i32x3(floor(grid_position)), i32x3(0), i32x3(volume.counts) - 1);
    let alpha = saturate(grid_position - f32x3(base_coord));

    var sum = f32x3(0.0);
    var specular_sum = f32x3(0.0);
    var weight_sum = 0.0;
    var active_probes = 0u;

    for (u32 corner = 0; corner < 8; corner++) {
        let offset = i32x3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        let probe_coord = u32x3(clamp(base_coord + offset, i32x3(0), i32x3(volume.counts) - 1));
        let probe_index = volume.probe_index_at(probe_coord);
        let probe_row = volume.probe_offset + probe_index;
        let state = states[probe_row];
        if (probe_is_inactive(state.flags)) {
            continue;
        }

        active_probes += 1;

        let probe_position = grid_min + f32x3(probe_coord) * volume.spacing + state.offset;

        let trilinear = lerp(1.0 - alpha, alpha, f32x3(offset));
        let trilinear_weight = trilinear.x * trilinear.y * trilinear.z;

        let to_probe = normalize(probe_position - world_position);
        let facing = (dot(to_probe, normal) + 1.0) * 0.5;
        var weight = facing * facing + 0.2;

        let bias_vector = biased_position - probe_position;
        let probe_distance = length(bias_vector);
        let moments = sample_probe_distance(distance_atlas, atlas_sampler, probe_row, bias_vector);
        let mean = moments.x;
        if (probe_distance > mean) {
            let variance = abs(mean * mean - moments.y);
            let difference = probe_distance - mean;
            let chebyshev = variance / (variance + difference * difference);
            weight *= chebyshev * chebyshev * chebyshev;
        }

        weight = max(weight, 1e-6);

        const f32 crush_threshold = 0.2;
        if (weight < crush_threshold) {
            weight *= (weight * weight) / (crush_threshold * crush_threshold);
        }

        weight *= trilinear_weight;

        sum += sample_probe_irradiance(irradiance_atlas, atlas_sampler, probe_row, normal) * weight;
        if (want_specular) {
            specular_sum += sample_probe_irradiance(irradiance_atlas, atlas_sampler, probe_row, specular_direction) *
                            weight;
        }

        weight_sum += weight;
    }

    if (active_probes == 0) {
        return result;
    }

    result.coverage = 1.0;

    if (weight_sum < MIN_TOTAL_WEIGHT) {
        return result;
    }

    result.irradiance = com::sanitize(sum / weight_sum);
    result.specular = com::sanitize(specular_sum / weight_sum);

    return result;
}

public func sample_ddgi_irradiance(
    StructuredBuffer<ProbeVolume> volumes,
    StructuredBuffer<ProbeState> states,
    u32 volume_count,
    Image2D<f32x4> irradiance_atlas,
    Image2D<f32x2> distance_atlas,
    Sampler atlas_sampler,
    f32x3 world_position,
    f32x3 normal,
    f32x3 view_direction,
    f32x3 specular_direction,
    bool want_specular,
    f32 normal_bias,
    f32 view_bias
) -> IrradianceSample {
    IrradianceSample result = { f32x3(0.0), f32x3(0.0), 0.0 };

    var remaining = 1.0;
    for (u32 volume_index = 0; volume_index < volume_count; volume_index++) {
        let volume = volumes[volume_index];
        let coverage = volume.blend_weight(world_position) * remaining;
        if (coverage <= 0.0) {
            continue;
        }

        let sample = sample_probe_volume(
            volume,
            states,
            irradiance_atlas,
            distance_atlas,
            atlas_sampler,
            world_position,
            normal,
            view_direction,
            specular_direction,
            want_specular,
            normal_bias,
            view_bias
        );
        if (sample.coverage <= 0.0) {
            continue;
        }

        result.irradiance += sample.irradiance * coverage;
        result.specular += sample.specular * coverage;
        result.coverage += coverage;

        remaining -= coverage;
        if (remaining <= 1e-3) {
            break;
        }
    }

    if (result.coverage > 0.0) {
        result.irradiance /= result.coverage;
        result.specular /= result.coverage;
    }

    return result;
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
