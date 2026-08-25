import common;
import gpu;

public constexpr static u32 IRRADIANCE_TEXELS = 12;
public constexpr static u32 RADIANCE_TEXELS = 6;
public constexpr static u32 DISTANCE_TEXELS = 12;
public constexpr static u32 PROBES_PER_ATLAS_ROW = 512;
public constexpr static u32 IRRADIANCE_ATLAS_WIDTH = PROBES_PER_ATLAS_ROW * (IRRADIANCE_TEXELS + 2);
public constexpr static u32 RADIANCE_PROBES_PER_ATLAS_ROW = IRRADIANCE_ATLAS_WIDTH / (RADIANCE_TEXELS + 2);
public constexpr static u32 PROBE_SELECT_GROUP = 64;
public constexpr static u32 TEXEL_UPDATE_GROUP = 8;
public constexpr static u32 TEXEL_UPDATE_THREADS_Y = 48;
public constexpr static f32 DISTANCE_SHARPNESS = 50.0;
public constexpr static f32 IRRADIANCE_ENCODING_GAMMA = 5.0;

public constexpr static bool SHARP_DISTANCE = true;
public constexpr static f32 DISTANCE_VARIANCE_BIAS = 0.01;

public constexpr static u32 DEBUG_SPHERE_RINGS = 8;
public constexpr static u32 DEBUG_SPHERE_SECTORS = 12;
public constexpr static u32 DEBUG_SPHERE_VERTEX_COUNT = DEBUG_SPHERE_RINGS * DEBUG_SPHERE_SECTORS * 6;

public static const u32 PROBE_INACTIVE_BIT = 1u;
public static const u32 PROBE_UPDATE_BIT = 2u;
public static const u32 PROBE_RESET_BIT = 4u;
public static const u32 PROBE_INSIDE_GEOMETRY_BIT = 8u;
public static const u32 PROBE_MESH_DISTANCE_BIT = 16u;
public static const u32 PROBE_MESH_DISTANCE_GRACE_SHIFT = 8u;
public static const u32 PROBE_MESH_DISTANCE_GRACE_MASK = 0x0f00u;
public static const u32 PROBE_MESH_DISTANCE_GRACE_UPDATES = 8u;
public static const u32 PROBE_MESH_DISTANCE_RECHECK_INTERVAL = 32u;

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

public func probe_is_inside_geometry(u32 flags) -> bool {
    return (flags & PROBE_INSIDE_GEOMETRY_BIT) != 0;
}

public func probe_is_mesh_distant(u32 flags) -> bool {
    return (flags & PROBE_MESH_DISTANCE_BIT) != 0;
}

public func probe_mesh_distance_grace(u32 flags) -> u32 {
    return (flags & PROBE_MESH_DISTANCE_GRACE_MASK) >> PROBE_MESH_DISTANCE_GRACE_SHIFT;
}

public func probe_pack_flags(
    bool inside_geometry,
    bool mesh_distant,
    bool update,
    bool reset,
    u32 mesh_distance_grace
) -> u32 {
    let inactive = inside_geometry || mesh_distant;
    return (inactive ? PROBE_INACTIVE_BIT : 0u) |
           (update ? PROBE_UPDATE_BIT : 0u) |
           (reset ? PROBE_RESET_BIT : 0u) |
           (inside_geometry ? PROBE_INSIDE_GEOMETRY_BIT : 0u) |
           (mesh_distant ? PROBE_MESH_DISTANCE_BIT : 0u) |
           ((mesh_distance_grace << PROBE_MESH_DISTANCE_GRACE_SHIFT) & PROBE_MESH_DISTANCE_GRACE_MASK);
}

public struct ProbeUpdateArgs {
    public u32 count;
    public DispatchIndirectCommand irradiance;
    public DispatchIndirectCommand distance;
    public DispatchIndirectCommand relocate;
};

public func probe_pack_update_entry(u32 volume_index, u32 probe_index) -> u32 {
    return (volume_index << 24u) | (probe_index & 0x00ffffffu);
}

public func probe_update_entry_volume(u32 entry) -> u32 {
    return entry >> 24u;
}

public func probe_update_entry_index(u32 entry) -> u32 {
    return entry & 0x00ffffffu;
}

public struct ProbeTexelWork {
    public u32 list_index;
    public u32x2 texel;
};

public func probe_texel_work(u32x2 thread, u32 interior_texels) -> ProbeTexelWork {
    let probes_per_column = TEXEL_UPDATE_THREADS_Y / interior_texels;

    ProbeTexelWork work;
    work.list_index = (thread.x / interior_texels) * probes_per_column + thread.y / interior_texels;
    work.texel = u32x2(thread.x % interior_texels, thread.y % interior_texels);

    return work;
}

public struct ProbeVolume {
    public f32x3 origin;
    public u32 probe_offset;
    public f32x3 spacing;
    public u32 probe_count;
    public f32x3 spacing_rcp;
    public u32 cascade_index;
    public u32x3 counts;
    public u32 cascade_count;
    public i32x3 scroll;
    public f32 cascade_blend;
    public f32x3 center;
    public f32 max_probe_distance;

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

    public func grid_coord_of(f32x3 world_position) -> f32x3 {
        return (world_position - this.grid_min()) * this.spacing_rcp;
    }

    public func coverage_weight(f32x3 world_position) -> f32 {
        let outside = max(abs(world_position - this.origin) - this.extents(), f32x3(0.0));
        let fade = 1.0 - smoothstep(0.0, 1.0, outside * this.spacing_rcp);
        return fade.x * fade.y * fade.z;
    }
};

public func cascade_safe_range(in ProbeVolume cascade_zero) -> f32x3 {
    return cascade_zero.spacing * max(f32x3(cascade_zero.counts / 2u) - 2.0, f32x3(1.0));
}

public func select_cascade(in ProbeVolume cascade_zero, f32x3 world_position) -> f32 {
    let normalized = abs(world_position - cascade_zero.center) / cascade_safe_range(cascade_zero);
    let radius = max(max(normalized.x, normalized.y), normalized.z);
    let cascade = max(0.0, log2(max(radius, 1e-6)) + 1.0);

    let outer = ceil(cascade);
    let blend_start = min(floor(cascade) + (1.0 - cascade_zero.cascade_blend), outer - 1e-4);

    return lerp(floor(cascade), outer, smoothstep(blend_start, outer, cascade));
}

public func probe_texel_direction(u32x2 texel, u32 interior_texels) -> f32x3 {
    return com::oct_to_vec3((f32x2(texel) + 0.5) / f32(interior_texels) * 2.0 - 1.0);
}

public func probe_tile_origin(u32 probe_row, u32 interior_texels) -> u32x2 {
    let tile = interior_texels + 2;
    return u32x2(probe_row % PROBES_PER_ATLAS_ROW, probe_row / PROBES_PER_ATLAS_ROW) * tile;
}

public func ray_data_row_origin(u32 probe_row, u32 rays_per_probe, u32 ray_data_width) -> u32x2 {
    let probes_per_row = max(ray_data_width / rays_per_probe, 1u);
    return u32x2((probe_row % probes_per_row) * rays_per_probe, probe_row / probes_per_row);
}

public func probe_atlas_uv(u32 probe_row, f32x3 direction, u32 interior_texels, f32x2 atlas_size) -> f32x2 {
    let origin = f32x2(probe_tile_origin(probe_row, interior_texels));
    let octant = com::vec3_to_oct(normalize(direction)) * 0.5 + 0.5;

    return (origin + 1.0 + octant * f32(interior_texels)) / atlas_size;
}

public func encode_probe_irradiance(f32x3 irradiance) -> f32x3 {
    return pow(max(irradiance, 0.0), 1.0 / IRRADIANCE_ENCODING_GAMMA);
}

public func decode_probe_irradiance(f32x3 encoded) -> f32x3 {
    let root = pow(max(encoded, 0.0), IRRADIANCE_ENCODING_GAMMA * 0.5);
    return root * root;
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

public func probe_radiance_tile_origin(u32 probe_row, u32 atlas_y_offset) -> u32x2 {
    let tile = RADIANCE_TEXELS + 2;
    return u32x2(
        (probe_row % RADIANCE_PROBES_PER_ATLAS_ROW) * tile,
        atlas_y_offset + (probe_row / RADIANCE_PROBES_PER_ATLAS_ROW) * tile
    );
}

public func load_probe_radiance(
    Image2D<f32x4> atlas,
    u32 probe_row,
    u32 atlas_y_offset,
    f32x3 direction
) -> f32x3 {
    let origin = probe_radiance_tile_origin(probe_row, atlas_y_offset);
    let octant = saturate(com::vec3_to_oct(normalize(direction)) * 0.5 + 0.5);
    let texel = min(
        u32x2(octant * f32(RADIANCE_TEXELS)),
        u32x2(RADIANCE_TEXELS - 1u)
    );

    return atlas.Load(u32x3(origin + 1 + texel, 0)).rgb;
}

public struct ProbeBorderWrites {
    public bool has_x;
    public bool has_y;
    public bool has_corner;
    public u32x2 x_texel;
    public u32x2 y_texel;
    public u32x2 corner_texel;
};

public func probe_border_writes(u32x2 texel, u32 interior_texels) -> ProbeBorderWrites {
    let last = interior_texels + 1;
    let tile_texel = texel + 1;

    ProbeBorderWrites writes;
    writes.has_x = tile_texel.x == 1 || tile_texel.x == interior_texels;
    writes.has_y = tile_texel.y == 1 || tile_texel.y == interior_texels;
    writes.has_corner = writes.has_x && writes.has_y;
    writes.x_texel = u32x2(tile_texel.x == 1 ? 0 : last, last - tile_texel.y);
    writes.y_texel = u32x2(last - tile_texel.x, tile_texel.y == 1 ? 0 : last);
    writes.corner_texel = u32x2(tile_texel.x == 1 ? last : 0, tile_texel.y == 1 ? last : 0);

    return writes;
}

// a word from our clanker:
// A fresh random rotation every frame gives the accumulator an independent ray set each update, so the
// probe's history random walks - slow visible breathing rather than noise the eye averages away. A short
// repeating low discrepancy sequence makes the estimate periodic instead, and the hysteresis filters that
// down to a small ripple: about 4x less wander for the same rate.
//
// 11 is the smallest period coprime with every update interval ddgi_select_probes can pick (1..8); on a
// shared factor a probe locks onto one rotation forever and bakes its sampling error into a fixed
// blotch. Smallest is best - a shorter period ripples faster, where the hysteresis attenuates it harder.
public constexpr static u32 RAY_ROTATION_PERIOD = 11;

func halton(u32 index, u32 base) -> f32 {
    var result = 0.0;
    var fraction = 1.0;
    var i = index;
    while (i > 0u) {
        fraction /= f32(base);
        result += fraction * f32(i % base);
        i /= base;
    }
    return result;
}

func rotate_by_quat(f32x4 q, f32x3 v) -> f32x3 {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

public func probe_ray_rotation(u32 frame_index) -> f32x4 {
    let index = frame_index % RAY_ROTATION_PERIOD + 1u;
    let u1 = halton(index, 2u);
    let u2 = halton(index, 3u);
    let u3 = halton(index, 5u);

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

public func probe_ray_direction(u32 ray_index, u32 ray_count, f32x4 rotation) -> f32x3 {
    return normalize(rotate_by_quat(rotation, spherical_fibonacci(ray_index, ray_count)));
}

public func probe_ray_direction(u32 ray_index, u32 ray_count, u32 frame_index) -> f32x3 {
    return probe_ray_direction(ray_index, ray_count, probe_ray_rotation(frame_index));
}

public func probe_world_position(in ProbeVolume volume, StructuredBuffer<ProbeState> states, u32 probe_index)
    -> f32x3 {
    return volume.probe_position(probe_index) + states[volume.probe_offset + probe_index].offset;
}

public struct IrradianceSample {
    public f32x3 irradiance;
    public f32x3 specular;
    public f32 coverage;
};

public struct RadianceSample {
    public f32x3 radiance;
    public f32 coverage;
};

public struct ProbeSampleParams {
    public f32x3 world_position;
    public f32x3 normal;
    public f32x3 geometric_normal;
    public f32x3 view_direction;
    public f32 view_distance;
    public f32x3 specular_direction;
    public bool want_specular;
    public f32 normal_bias;
    public f32 view_bias;

    public func biased_position(in ProbeVolume volume) -> f32x3 {
        let cell = min(min(volume.spacing.x, volume.spacing.y), volume.spacing.z);
        let reach = min(cell, this.view_distance * 0.5);
        let direction = this.normal_bias * this.geometric_normal + this.view_bias * this.view_direction;

        return this.world_position + direction * reach;
    }
};

public func sample_probe_volume(
    in ProbeVolume volume,
    StructuredBuffer<ProbeState> states,
    Image2D<f32x4> irradiance_atlas,
    Image2D<f32x2> distance_atlas,
    Sampler atlas_sampler,
    in ProbeSampleParams params
) -> IrradianceSample {
    IrradianceSample result = { f32x3(0.0), f32x3(0.0), 0.0 };

    let biased_position = params.biased_position(volume);
    let grid_min = volume.grid_min();
    let grid_position = volume.grid_coord_of(biased_position);
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

        let trilinear = max(lerp(1.0 - alpha, alpha, f32x3(offset)), 0.001);
        let trilinear_weight = trilinear.x * trilinear.y * trilinear.z;

        let to_probe = normalize(probe_position - params.world_position);
        let facing = (dot(to_probe, params.normal) + 1.0) * 0.5;
        var weight = facing * facing + 0.2;

        let bias_vector = biased_position - probe_position;
        let probe_distance = length(bias_vector);
        let moments = sample_probe_distance(distance_atlas, atlas_sampler, probe_row, bias_vector);
        let mean = moments.x;
        var chebyshev = 1.0;
        if (probe_distance > mean) {
            let variance_floor = DISTANCE_VARIANCE_BIAS * volume.max_probe_distance;
            let variance = max(abs(mean * mean - moments.y), variance_floor * variance_floor);
            let difference = probe_distance - mean;
            chebyshev = variance / (variance + difference * difference);
            chebyshev = chebyshev * chebyshev * chebyshev;
        }

        weight *= max(chebyshev, 0.05);
        weight = max(weight, 1e-6);

        const f32 crush_threshold = 0.2;
        if (weight < crush_threshold) {
            weight *= (weight * weight) / (crush_threshold * crush_threshold);
        }

        weight *= trilinear_weight;

        let half_decode = IRRADIANCE_ENCODING_GAMMA * 0.5;
        sum += pow(
                   sample_probe_irradiance(irradiance_atlas, atlas_sampler, probe_row, params.normal),
                   half_decode
               ) *
               weight;
        if (params.want_specular) {
            specular_sum += pow(
                                sample_probe_irradiance(
                                    irradiance_atlas,
                                    atlas_sampler,
                                    probe_row,
                                    params.specular_direction
                                ),
                                half_decode
                            ) *
                            weight;
        }

        weight_sum += weight;
    }

    if (active_probes == 0 || weight_sum <= 0.0) {
        return result;
    }

    result.coverage = weight_sum;

    let irradiance_root = com::sanitize(sum / weight_sum);
    let specular_root = com::sanitize(specular_sum / weight_sum);
    result.irradiance = irradiance_root * irradiance_root;
    result.specular = specular_root * specular_root;

    return result;
}

public func sample_probe_radiance_volume(
    in ProbeVolume volume,
    StructuredBuffer<ProbeState> states,
    Image2D<f32x4> irradiance_atlas,
    Image2D<f32x2> distance_atlas,
    Sampler atlas_sampler,
    u32 radiance_atlas_y_offset,
    in ProbeSampleParams params
) -> RadianceSample {
    RadianceSample result = { f32x3(0.0), 0.0 };

    let biased_position = params.biased_position(volume);
    let grid_min = volume.grid_min();
    let grid_position = volume.grid_coord_of(biased_position);
    let base_coord = clamp(i32x3(floor(grid_position)), i32x3(0), i32x3(volume.counts) - 1);
    let alpha = saturate(grid_position - f32x3(base_coord));

    var sum = f32x3(0.0);
    var weight_sum = 0.0;

    for (u32 corner = 0; corner < 8; corner++) {
        let offset = i32x3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        let probe_coord = u32x3(clamp(base_coord + offset, i32x3(0), i32x3(volume.counts) - 1));
        let probe_index = volume.probe_index_at(probe_coord);
        let probe_row = volume.probe_offset + probe_index;
        let state = states[probe_row];
        if (probe_is_inactive(state.flags) || probe_is_reset(state.flags)) {
            continue;
        }

        let probe_position = grid_min + f32x3(probe_coord) * volume.spacing + state.offset;

        let trilinear = max(lerp(1.0 - alpha, alpha, f32x3(offset)), 0.001);
        let trilinear_weight = trilinear.x * trilinear.y * trilinear.z;

        let to_probe = normalize(probe_position - params.world_position);
        let facing = (dot(to_probe, params.normal) + 1.0) * 0.5;
        var weight = facing * facing;

        let bias_vector = biased_position - probe_position;
        let probe_distance = length(bias_vector);
        let moments = sample_probe_distance(distance_atlas, atlas_sampler, probe_row, bias_vector);
        let mean = moments.x;
        var chebyshev = 1.0;
        if (probe_distance > mean) {
            let variance_floor = DISTANCE_VARIANCE_BIAS * volume.max_probe_distance;
            let variance = max(abs(mean * mean - moments.y), variance_floor * variance_floor);
            let difference = probe_distance - mean;
            chebyshev = variance / (variance + difference * difference);
            chebyshev = chebyshev * chebyshev * chebyshev;
        }

        weight *= max(chebyshev, 0.05);
        weight *= trilinear_weight;

        let direction = params.world_position - probe_position;
        let radiance = max(
            load_probe_radiance(irradiance_atlas, probe_row, radiance_atlas_y_offset, direction),
            f32x3(0.0)
        );
        sum += sqrt(radiance) * weight;
        weight_sum += weight;
    }

    if (weight_sum <= 0.0) {
        return result;
    }

    const f32 min_weight = 0.1;
    let root = com::sanitize(sum / max(weight_sum, min_weight));
    result.radiance = root * root;
    result.coverage = weight_sum;

    return result;
}

public static const f32 CASCADE_FALLBACK_WEIGHT = 0.02;

public func sample_ddgi_radiance(
    StructuredBuffer<ProbeVolume> volumes,
    StructuredBuffer<ProbeState> states,
    u32 volume_count,
    Image2D<f32x4> irradiance_atlas,
    Image2D<f32x2> distance_atlas,
    Sampler atlas_sampler,
    u32 radiance_atlas_y_offset,
    in ProbeSampleParams params
) -> RadianceSample {
    RadianceSample result = { f32x3(0.0), 0.0 };

    var remaining = 1.0;
    for (u32 set_start = 0; set_start < volume_count;) {
        let cascade_zero = volumes[set_start];
        let set_count = max(cascade_zero.cascade_count, 1u);
        let outer_coverage = volumes[set_start + set_count - 1u].coverage_weight(params.world_position) *
                             remaining;
        if (outer_coverage <= 0.0) {
            set_start += set_count;
            continue;
        }

        let cascade = min(select_cascade(cascade_zero, params.world_position), f32(set_count - 1u));

        let lower = sample_probe_radiance_volume(
            volumes[set_start + u32(floor(cascade))],
            states,
            irradiance_atlas,
            distance_atlas,
            atlas_sampler,
            radiance_atlas_y_offset,
            params
        );

        if (lower.coverage > 0.0) {
            result.radiance += lower.radiance * outer_coverage;
            result.coverage += outer_coverage;
            remaining -= outer_coverage;
            if (remaining <= 1e-3) {
                break;
            }
        }

        set_start += set_count;
    }

    if (result.coverage > 0.0) {
        result.radiance /= result.coverage;
    }

    return result;
}

public func sample_ddgi_irradiance(
    StructuredBuffer<ProbeVolume> volumes,
    StructuredBuffer<ProbeState> states,
    u32 volume_count,
    Image2D<f32x4> irradiance_atlas,
    Image2D<f32x2> distance_atlas,
    Sampler atlas_sampler,
    in ProbeSampleParams params
) -> IrradianceSample {
    IrradianceSample result = { f32x3(0.0), f32x3(0.0), 0.0 };

    var remaining = 1.0;
    for (u32 set_start = 0; set_start < volume_count;) {
        let cascade_zero = volumes[set_start];
        let set_count = max(cascade_zero.cascade_count, 1u);
        let outer_coverage = volumes[set_start + set_count - 1u].coverage_weight(params.world_position) *
                             remaining;
        if (outer_coverage <= 0.0) {
            set_start += set_count;
            continue;
        }

        let cascade = min(select_cascade(cascade_zero, params.world_position), f32(set_count - 1u));

        let lower_index = u32(floor(cascade));
        let lower = sample_probe_volume(
            volumes[set_start + lower_index],
            states,
            irradiance_atlas,
            distance_atlas,
            atlas_sampler,
            params
        );

        var irradiance = lower.irradiance;
        var specular = lower.specular;
        var weight = lower.coverage;

        let higher_index = lower_index + 1;
        let lower_missed = weight < CASCADE_FALLBACK_WEIGHT;
        if (higher_index < set_count && (lower_missed || frac(cascade) > 0.0)) {
            let higher = sample_probe_volume(
                volumes[set_start + higher_index],
                states,
                irradiance_atlas,
                distance_atlas,
                atlas_sampler,
                params
            );

            let higher_missed = higher.coverage < CASCADE_FALLBACK_WEIGHT;
            var blend = frac(cascade);
            if (lower_missed != higher_missed) {
                blend = lower_missed ? 1.0 : 0.0;
            }

            irradiance = lerp(irradiance, higher.irradiance, blend);
            specular = lerp(specular, higher.specular, blend);
            weight = lerp(weight, higher.coverage, blend);
        }

        if (weight > 0.0) {
            let coverage = saturate(weight / CASCADE_FALLBACK_WEIGHT) * outer_coverage;
            result.irradiance += irradiance * coverage;
            result.specular += specular * coverage;
            result.coverage += coverage;
            remaining -= coverage;
            if (remaining <= 1e-3) {
                break;
            }
        }

        set_start += set_count;
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
