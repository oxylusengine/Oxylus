module sky;

import gpu;
import common;
import scene;

public func from_sub_uvs_to_unit(f32x2 uv, f32x2 res) -> f32x2 {
    return (uv - 0.5 / res) * (res / (res - 1.0));
}

public func from_unit_to_sub_uvs(f32x2 uv, f32x2 res) -> f32x2 {
    return (uv + 0.5 / res) * (res / (res + 1.0));
}

public func transmittance_params_to_lut_uv(Atmosphere *atmosphere, f32x2 p) -> f32x2 {
    f32 h = com::safe_sqrt(
        atmosphere->atmos_radius * atmosphere->atmos_radius -
        atmosphere->planet_radius * atmosphere->planet_radius);
    f32 rho = com::safe_sqrt(p.x * p.x - atmosphere->planet_radius * atmosphere->planet_radius);
    f32 discriminant = p.x * p.x * (p.y * p.y - 1.0) +
        atmosphere->atmos_radius * atmosphere->atmos_radius;
    f32 d = max(0.0, -p.x *  p.y + com::safe_sqrt(discriminant));
    f32 d_min = atmosphere->atmos_radius - p.x;
    f32 d_max = rho + h;
    f32 mu = (d - d_min) / (d_max - d_min);
    f32 r = rho / h;

    return f32x2(mu, r);
}

public func multiscattering_params_to_lut_uv(Atmosphere *atmosphere, f32 altitude, f32 cos_theta) -> f32x2 {
    f32x2 uv = clamp(f32x2(
        cos_theta * 0.5 + 0.5,
        altitude / (atmosphere->atmos_radius - atmosphere->planet_radius)),
        0.0, 1.0);
    return from_unit_to_sub_uvs(uv, f32x2(atmosphere->multiscattering_lut_size.xy));
}

public func sky_view_params_to_lut_uv(
    Atmosphere *atmosphere,
    bool intersect_planet,
    f32 altitude,
    f32 view_zenith_cos_angle,
    f32x2 light_on_plane
) -> f32x2 {
    f32 horizon = com::safe_sqrt(altitude * altitude - atmosphere->planet_radius * atmosphere->planet_radius);
    f32 beta = acos(horizon / altitude);
    f32 zenith_horizon_angle = PI - beta;
    f32 view_zenith_angle = acos(view_zenith_cos_angle);

    f32x2 uv = 0.0;
    if (!intersect_planet) {
        f32 coord = view_zenith_angle / zenith_horizon_angle;
        coord = 1.0 - coord;
        coord = com::safe_sqrt(coord);
        coord = 1.0 - coord;
        uv.y = coord * 0.5;
    } else {
        f32 coord = (view_zenith_angle - zenith_horizon_angle) / beta;
        coord = com::safe_sqrt(coord);
        uv.y = coord * 0.5 + 0.5;
    }

    f32 theta = atan2(-light_on_plane.y, -light_on_plane.x);
    uv.x = (theta + PI) / (2.0 * PI);
    return from_unit_to_sub_uvs(uv, f32x2(atmosphere->sky_view_lut_size.xy));
}

public func uv_to_sky_view_lut_params(
    Atmosphere *atmosphere,
    f32x2 uv,
    f32 altitude
) -> f32x3 {
    uv = from_sub_uvs_to_unit(uv, f32x2(atmosphere->sky_view_lut_size.xy));
    f32 horizon = com::safe_sqrt(altitude * altitude - atmosphere->planet_radius * atmosphere->planet_radius);
    f32 beta = acos(horizon / altitude);
    f32 zenith_horizon_angle = PI - beta;

    f32 view_zenith_angle = 0.0;
    if (uv.y < 0.5) {
        f32 coord = uv.y * 2.0;
        coord = 1.0 - coord;
        coord *= coord;
        coord = 1.0 - coord;
        view_zenith_angle = zenith_horizon_angle * coord;
    } else {
        f32 coord = uv.y * 2.0 - 1.0;
        coord *= coord;
        view_zenith_angle = zenith_horizon_angle + beta * coord;
    }

    const f32 longitude_view_cos_angle = uv.x * TAU;
    const f32 view_zenith_cos_angle = cos(view_zenith_angle);
    const f32 view_zenith_sin_angle = com::safe_sqrt(1.0 - view_zenith_cos_angle * view_zenith_cos_angle) * (view_zenith_angle > 0.0 ? 1.0 : -1.0);
    const f32 cos_longitude_view_cos_angle = cos(longitude_view_cos_angle);
    const f32 sin_longitude_view_cos_angle = com::safe_sqrt(1.0 - cos_longitude_view_cos_angle * cos_longitude_view_cos_angle) * (longitude_view_cos_angle <= PI ? 1.0 : -1.0);
    return f32x3(
        view_zenith_sin_angle * cos_longitude_view_cos_angle,
        view_zenith_cos_angle,
        view_zenith_sin_angle * sin_longitude_view_cos_angle
    );
}

public func move_to_top_atmosphere(inout f32x3 pos, f32x3 dir, f32 atmos_radius) -> bool {
    f32 h = length(pos);
    if (h > atmos_radius) {
        const let top_intersection = com::ray_sphere_intersect_nearest(pos, dir, atmos_radius);
        if (top_intersection.hasValue) {
            f32x3 up_vec = pos / h;
            f32x3 up_offset = up_vec * -PLANET_RADIUS_OFFSET;
            pos = pos + dir * top_intersection.value + up_offset;
        } else {
            return false;
        }
    }
    return true;
}

public struct MediumScattering {
    public f32x3 rayleigh_scattering;
    public f32x3 rayleigh_extinction;

    public f32x3 mie_scattering;
    public f32x3 mie_extinction;

    public f32x3 ozone_absorption;
    public f32x3 ozone_extinction;

    public f32x3 scattering_sum;
    public f32x3 extinction_sum;

    [ForceInline]
    public __init(Atmosphere *atmosphere, f32 altitude) {
        const f32 rayleigh_density  = exp(-altitude / atmosphere->rayleigh_density);
        const f32 mie_density = exp(-altitude / atmosphere->mie_density);
        const f32 ozone_density = max(0.0, 1.0 - abs(altitude - atmosphere->ozone_height) / atmosphere->ozone_thickness);

        this.rayleigh_scattering = atmosphere->rayleigh_scatter * rayleigh_density;
        this.rayleigh_extinction = this.rayleigh_scattering;  // Rayleigh scattering doesn't have absorption behavior

        this.mie_scattering = atmosphere->mie_scatter * mie_density;
        this.mie_extinction = atmosphere->mie_extinction * mie_density;  // Mie scattering doesn't have absorption behavior

        this.ozone_absorption = atmosphere->ozone_absorption * ozone_density;
        this.ozone_extinction = this.ozone_absorption;

        this.scattering_sum = this.rayleigh_scattering + this.mie_scattering;
        this.extinction_sum = this.rayleigh_extinction + this.mie_extinction + this.ozone_extinction;
    }
};

public struct AtmosphereLuminance {
    public f32x3 luminance = 0.0;
    public f32x3 multiscattering_as_1 = 0.0;
    public f32x3 transmittance = 1.0;
};

public struct AtmosphereSampling {
    public bool variable_sample_count = false;
    public f32  initial_sample_count = 0.0;
    public f32  min_sample_count = 0.0;
    public f32  max_sample_count = 0.0;
    public f32  inv_distance_to_sample_count_max = 0.01;
};

public struct AtmosphereIntegrateInfo {
    public f32x3 eye_pos = {};
    public f32x3 eye_dir = {};
    public f32x3 sun_dir = {};
    public f32   sun_intensity = 1.0;
    public f32   max_integration_length = 9000000.0;
    public AtmosphereSampling sampling = {};
    public constexpr bool eval_mie_phase = true;
    public constexpr bool eval_rayleigh_phase = true;
    public constexpr bool eval_planet_luminance = false;

    public Image2D<f32x4> transmittance_image = {};
    public Optional<Image2D<f32x4>> multiscattering_image = none;
};

public func integrate_single_scattered_luminance(
    Atmosphere *atmosphere,
    in Sampler lut_sampler,
    in AtmosphereIntegrateInfo info
) -> AtmosphereLuminance {
    AtmosphereLuminance result = {};

    if (dot(info.eye_pos, info.eye_pos) <= atmosphere->planet_radius * atmosphere->planet_radius) {
        return result;
    }

    const let atmos_intersection = com::ray_sphere_intersect_nearest(info.eye_pos, info.eye_dir, atmosphere->atmos_radius);
    const let planet_intersection = com::ray_sphere_intersect_nearest(info.eye_pos, info.eye_dir, atmosphere->planet_radius);
    f32 integration_length = 0.0;
    if (!atmos_intersection.hasValue) {
        // No intersection
        return result;
    } else if (!planet_intersection.hasValue) {
        // Atmosphere only intersection
        integration_length = atmos_intersection.value;
    } else {
        integration_length = max(0.0, planet_intersection.value);
    }

    integration_length = min(integration_length, info.max_integration_length);
    f32 sample_count = info.sampling.initial_sample_count;
    f32 sample_count_floor = info.sampling.initial_sample_count;
    f32 max_integration_length_floor = integration_length;
    if (info.sampling.variable_sample_count) {
        sample_count = lerp(info.sampling.min_sample_count, info.sampling.max_sample_count,
                            saturate(integration_length * info.sampling.inv_distance_to_sample_count_max));
        sample_count_floor = floor(sample_count);
        max_integration_length_floor = integration_length * sample_count_floor / sample_count;
    }

    f32 cos_theta = dot(info.sun_dir, info.eye_dir);
    f32 rayleigh_phase = com::rayleigh_phase(cos_theta);
    f32 mie_phase = com::henyey_greenstein_draine_phase(atmosphere->mie_asymmetry, cos_theta);

    f32 step = 0.0;
    f32 delta_step = integration_length / sample_count;
    for (f32 i = 0; i < sample_count; i += 1.0) {
        if (info.sampling.variable_sample_count) {
            f32 cur_step  = (i + 0.0) / sample_count_floor;
            f32 next_step = (i + 1.0) / sample_count_floor;
            cur_step *= cur_step;
            next_step *= next_step;

            cur_step *= max_integration_length_floor;
            next_step = next_step > 1.0 ? integration_length : next_step * max_integration_length_floor;
            delta_step = next_step - cur_step;
            step = cur_step + delta_step * 0.3;
        } else {
            step = integration_length * (i + 0.3) / sample_count;
        }

        f32x3 step_pos = info.eye_pos + step * info.eye_dir;
        f32 h = length(step_pos);
        f32 altitude = h - atmosphere->planet_radius;
        MediumScattering medium_info = MediumScattering(atmosphere, altitude);

        f32x3 up_vec = normalize(step_pos);
        f32x3 up_vec_scaled = PLANET_RADIUS_OFFSET * up_vec;
        f32 earth_shadow = com::ray_sphere_intersect_nearest(step_pos - up_vec_scaled, info.sun_dir, atmosphere->planet_radius).hasValue ? 0.0 : 1.0;
        f32 sun_theta = dot(info.sun_dir, up_vec);

        f32x2 transmittance_uv = transmittance_params_to_lut_uv(atmosphere, f32x2(h, sun_theta));
        f32x3 sun_transmittance = info.transmittance_image.sample_mip(lut_sampler, transmittance_uv, 0.0).rgb;

        f32x3 MS = 0.0;
        if (info.multiscattering_image.hasValue) {
            f32x2 multiscatter_uv = multiscattering_params_to_lut_uv(atmosphere, altitude, sun_theta);
            MS = info.multiscattering_image.value.sample_mip(lut_sampler, multiscatter_uv, 0.0).rgb;
        }

        f32x3 scattering_phase = 0.0;
        if (info.eval_mie_phase && info.eval_rayleigh_phase) {
            scattering_phase = medium_info.mie_scattering * mie_phase + medium_info.rayleigh_scattering * rayleigh_phase;
        } else if (info.eval_mie_phase) {
            scattering_phase = medium_info.mie_scattering * mie_phase + medium_info.rayleigh_scattering * com::uniform_phase();
        } else if (info.eval_rayleigh_phase) {
            scattering_phase = medium_info.mie_scattering * com::uniform_phase() + medium_info.rayleigh_scattering * rayleigh_phase;
        } else {
            scattering_phase = medium_info.scattering_sum * com::uniform_phase();
        }

        f32x3 sun_luminance = earth_shadow * sun_transmittance * scattering_phase + (MS * medium_info.scattering_sum);
        f32x3 step_transmittance = exp(-delta_step * medium_info.extinction_sum);

        f32x3 integral = (sun_luminance - sun_luminance * step_transmittance) / medium_info.extinction_sum;
        f32x3 ms_integral = (medium_info.scattering_sum - medium_info.scattering_sum * step_transmittance) / medium_info.extinction_sum;

        result.luminance += info.sun_intensity * (integral * result.transmittance);
        result.multiscattering_as_1 += ms_integral * result.transmittance;
        result.transmittance *= step_transmittance;
    }

    // Accumulate light bouncing off planet's ground
    if (info.eval_planet_luminance && planet_intersection.hasValue && integration_length == planet_intersection.value) {
        f32x3 planet_pos = info.eye_pos + integration_length * info.eye_dir;
        f32 h = length(planet_pos);
        f32x3 up_vec = planet_pos / h;
        f32 sun_theta = dot(info.sun_dir, up_vec);
        f32 NoL = saturate(dot(normalize(info.sun_dir), normalize(up_vec)));

        f32x2 transmittance_uv = transmittance_params_to_lut_uv(atmosphere, f32x2(h, sun_theta));
        f32x3 sun_transmittance = info.transmittance_image.sample_mip(lut_sampler, transmittance_uv, 0.0).rgb;

        result.luminance += info.sun_intensity * (sun_transmittance * result.transmittance * NoL * atmosphere->terrain_albedo / PI);
    }

    return result;
}

public func sample_aerial_perspective(
    Atmosphere *atmosphere,
    in Image3D<f32x4> aerial_perspective_lut,
    in Sampler sampler,
    f32x2 uv,
    f32x3 camera_relative_pos
) -> f32x4 {
    const let lut_size = f32x3(atmosphere->aerial_perspective_lut_size);
    const let per_slice_depth = f32(lut_size.x / lut_size.z);
    const let aerial_perspective_lut_depth = lut_size.z;

    let relative_depth = max(0.0, length(camera_relative_pos) - atmosphere->aerial_perspective_start_km);
    let linear_slice = relative_depth * rcp(per_slice_depth);
    let linear_w = linear_slice * rcp(aerial_perspective_lut_depth);
    let non_linear_w = sqrt(linear_w);
    let non_linear_slice = non_linear_w * aerial_perspective_lut_depth;

    f32 weight = 1.0;
    const let half_slice_depth = 0.70710678118654752440084436210485f;  // sqrt(0.5f)
    if (non_linear_slice < half_slice_depth) {
        weight = saturate(non_linear_slice * non_linear_slice * 2.0);
    }

    const let near_depth_fade_out = 1.0 / 0.00001;
    weight *= saturate(relative_depth * near_depth_fade_out);

#if 0
    return f32x4(relative_depth, linear_slice, non_linear_w, linear_w);
#endif
    f32x4 aerial_perspective = aerial_perspective_lut.sample_mip(sampler, f32x3(uv, non_linear_w), 0.0);
    aerial_perspective.xyz *= weight;
    aerial_perspective.w = 1.0 - (weight * (1.0 - aerial_perspective.w));

    return aerial_perspective;
}
