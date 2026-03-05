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

public func transmittance_params_to_lut_uv(
    f32x2 transmittance_lut_size,
    f32 atmos_radius,
    f32 planet_radius, f32x2 p
) -> f32x2 {
    f32 h = com::safe_sqrt(
        atmos_radius * atmos_radius -
        planet_radius * planet_radius);
    f32 rho = com::safe_sqrt(p.x * p.x - planet_radius * planet_radius);
    f32 discriminant = p.x * p.x * (p.y * p.y - 1.0) +
        atmos_radius * atmos_radius;
    f32 d = max(0.0, -p.x *  p.y + com::safe_sqrt(discriminant));
    f32 d_min = atmos_radius - p.x;
    f32 d_max = rho + h;
    f32 mu = (d - d_min) / (d_max - d_min);
    f32 r = rho / h;

    return from_unit_to_sub_uvs(f32x2(mu, r), transmittance_lut_size);
}

public func multiscattering_params_to_lut_uv(in Atmosphere atmosphere, f32 altitude, f32 cos_theta) -> f32x2 {
    f32x2 uv = clamp(f32x2(
        cos_theta * 0.5 + 0.5,
        altitude / (atmosphere.atmos_radius - atmosphere.planet_radius)),
        0.0, 1.0);
    return from_unit_to_sub_uvs(uv, f32x2(atmosphere.multiscattering_lut_size.xy));
}

public func sky_view_params_to_lut_uv(
    in Atmosphere atmosphere,
    bool intersect_planet,
    f32 altitude,
    f32 view_zenith_angle,
    f32 light_view_angle
) -> f32x2 {
    var uv: f32x2;
    if (altitude < atmosphere.atmos_radius) {
        let beta = asin(atmosphere.planet_radius / altitude);
        let zenith_horizon_angle = PI - beta;
        if (!intersect_planet) {
            var coord = clamp(view_zenith_angle / zenith_horizon_angle, 0.0, 1.0);
            coord = (1.0 - com::safe_sqrt(1.0 - coord)) * 0.5;
            uv.y = coord;
        } else {
            var coord = clamp((view_zenith_angle - zenith_horizon_angle) / beta, 0.0, 1.0);
            coord = (com::safe_sqrt(coord) + 1.0) * 0.5;
            uv.y = coord;
        }
    } else {
        let beta = asin(atmosphere.atmos_radius / altitude);
        let zenith_horizon_angle = PI - beta;
        var coord = clamp((view_zenith_angle - zenith_horizon_angle) / beta, 0.0, 1.0);
        uv.y = com::safe_sqrt(coord);
    }

    uv.x = com::safe_sqrt(light_view_angle / PI);
    return from_unit_to_sub_uvs(uv, f32x2(atmosphere.sky_view_lut_size.xy));
}

public func uv_to_sky_view_lut_params(
    in Atmosphere atmosphere,
    f32x2 uv,
    f32 altitude
) -> f32x2 {
    uv = from_sub_uvs_to_unit(uv, f32x2(atmosphere.sky_view_lut_size.xy));
    var view_zenith_angle: f32;
    var light_view_angle: f32;

    if (altitude < atmosphere.atmos_radius) {
        let beta = asin(atmosphere.planet_radius / altitude);
        let zenith_horizon_angle = PI - beta;
        if (uv.y < 0.5) {
            let y = 1.0 - 2.0 * uv.y;
            let coord = 1.0 - y * y;
            view_zenith_angle = zenith_horizon_angle * coord;
        } else {
            let y = 2.0 * uv.y - 1.0;
            let coord = y * y;
            view_zenith_angle = zenith_horizon_angle + beta * coord;
        }
    } else {
        let beta = asin(atmosphere.atmos_radius / altitude);
        let zenith_horizon_angle = PI - beta;
        let coord = uv.y * uv.y;
        view_zenith_angle = zenith_horizon_angle + beta * coord;
    }

    light_view_angle = (uv.x * uv.x) * PI;
    return f32x2(view_zenith_angle, light_view_angle);
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

public struct Medium {
    public f32x3 rayleigh_scattering;
    public f32x3 mie_scattering;
    public f32x3 extinction;

    [ForceInline]
    public __init(in Atmosphere atmosphere, f32 altitude) {
        let mie_density = max(0.0, atmosphere.sample_density_profile(atmosphere.mie_density, altitude));
        let rayleigh_density = max(0.0, atmosphere.sample_density_profile(atmosphere.rayleigh_density, altitude));
        let ozone_density = max(0.0, atmosphere.sample_density_profile(atmosphere.absorption_density, altitude));

        let mie_extinction = atmosphere.mie_extinction * mie_density;
        let rayleigh_extinction = atmosphere.rayleigh_scatter * rayleigh_density;
        let ozone_extinction = atmosphere.absorption_extiction * ozone_density;

        this.rayleigh_scattering = atmosphere.rayleigh_scatter * rayleigh_density;
        this.mie_scattering = atmosphere.mie_scatter * mie_density;
        this.extinction = mie_extinction + rayleigh_extinction + ozone_extinction;
    }
};

public struct AtmosphereLuminance {
    public f32x3 luminance = 0.0;
    public f32x3 multiscattering_as_1 = 0.0;
    public f32x3 transmittance = 1.0;
};

public struct AtmosphereIntegrateInfo {
    public f32x3 eye_pos = {};
    public f32x3 eye_dir = {};
    public f32x3 sun_dir = {};
    public f32 step_count = 32.0;
    public f32 t_max = -1.0;
    public constexpr bool eval_planet_luminance = false;
    public constexpr bool eval_multiscattering = false;
};

public func integrate_single_scattered_luminance(
    in AtmosphereIntegrateInfo info,
    in Atmosphere atmosphere,
    in Sampler lut_sampler,
    in Image2D<f32x4> transmittance_image = {},
    in Image2D<f32x4> multiscattering_image = {}
) -> AtmosphereLuminance {
    AtmosphereLuminance result = {};

    if (dot(info.eye_pos, info.eye_pos) <= atmosphere.planet_radius * atmosphere.planet_radius) {
        return result;
    }

    let planet_intersection = com::ray_sphere_intersect_nearest(info.eye_pos, info.eye_dir, atmosphere.planet_radius);
    var integration_length = 0.0;
    if (info.t_max == -1.0) {
        let atmos_intersection = com::ray_sphere_intersect_nearest(info.eye_pos, info.eye_dir, atmosphere.atmos_radius);
        if (!atmos_intersection.hasValue) {
            // No intersection
            return result;
        } else if (!planet_intersection.hasValue) {
            // Atmosphere only intersection
            integration_length = atmos_intersection.value;
        } else {
            integration_length = max(0.0, planet_intersection.value);
        }
    } else {
        integration_length = info.t_max;
    }

    let cos_theta = dot(info.sun_dir, info.eye_dir);
    let rayleigh_phase = com::rayleigh_phase(cos_theta);
    let mie_phase = com::henyey_greenstein_draine_phase(atmosphere.mie_asymmetry, cos_theta);

    var step_length = integration_length / info.step_count;
    var step = integration_length * info.eye_dir / info.step_count;
    for (var i = 0.0; i < info.step_count; i += 1.0) {
        let step_pos = info.eye_pos + (i + 0.5) * step;
        let h = length(step_pos);
        let altitude = h - atmosphere.planet_radius;
        let medium_info = Medium(atmosphere, altitude);
        let medium_scattering = medium_info.rayleigh_scattering + medium_info.mie_scattering;
        let scattering_phase = medium_info.mie_scattering * mie_phase
                             + medium_info.rayleigh_scattering * rayleigh_phase;

        let up = normalize(step_pos);
        let sun_theta = dot(info.sun_dir, up);

        let transmittance_uv = transmittance_params_to_lut_uv(
            atmosphere.transmittance_lut_size.xy, atmosphere.atmos_radius, atmosphere.planet_radius, f32x2(h, sun_theta));
        let sun_transmittance = transmittance_image.SampleLevel(lut_sampler, transmittance_uv, 0.0).rgb;

        var ms_contribution = f32x3(0.0);
        if (info.eval_multiscattering) {
            let multiscatter_uv = multiscattering_params_to_lut_uv(atmosphere, altitude, sun_theta);
            let ms = multiscattering_image.SampleLevel(lut_sampler, multiscatter_uv, 0.0).rgb;
            ms_contribution = ms * medium_scattering;
        }

        let earth_shadow = com::ray_sphere_intersect_nearest(
            step_pos - PLANET_RADIUS_OFFSET * up, info.sun_dir, atmosphere.planet_radius).hasValue ? 0.0 : 1.0;
        let sun_luminance = earth_shadow * sun_transmittance * scattering_phase + ms_contribution;
        let step_transmittance = exp(-step_length * medium_info.extinction);

        let integral = (sun_luminance - sun_luminance * step_transmittance) / medium_info.extinction;
        let ms_integral = (medium_scattering - medium_scattering * step_transmittance) / medium_info.extinction;

        result.luminance += integral * result.transmittance;
        result.multiscattering_as_1 += ms_integral * result.transmittance;
        result.transmittance *= step_transmittance;
    }

    // Accumulate light bouncing off planet's ground
    if (info.eval_planet_luminance && planet_intersection.hasValue && integration_length == planet_intersection.value) {
        let planet_pos = info.eye_pos + integration_length * info.eye_dir;
        let h = length(planet_pos);
        let up = normalize(planet_pos);
        let sun_theta = dot(info.sun_dir, up);
        let NoL = saturate(dot(normalize(info.sun_dir), up));

        let transmittance_uv = transmittance_params_to_lut_uv(
            atmosphere.transmittance_lut_size.xy, atmosphere.atmos_radius, atmosphere.planet_radius, f32x2(h, sun_theta));
        let sun_transmittance = transmittance_image.SampleLevel(lut_sampler, transmittance_uv, 0.0).rgb;

        result.luminance += sun_transmittance * result.transmittance * NoL * atmosphere.terrain_albedo / PI;
    }

    return result;
}

public func sample_aerial_perspective(
    in Atmosphere atmosphere,
    in Image3D<f32x4> aerial_perspective_lut,
    in Sampler sampler,
    f32x2 uv,
    f32x3 camera_relative_pos
) -> f32x4 {
    let lut_size = f32x3(atmosphere.aerial_perspective_lut_size);
    let per_slice_depth = f32(lut_size.x / lut_size.z);
    let aerial_perspective_lut_depth = lut_size.z;

    let relative_depth = max(0.0, length(camera_relative_pos) - atmosphere.aerial_perspective_start_km);
    let linear_slice = relative_depth * rcp(per_slice_depth);
    let linear_w = linear_slice * rcp(aerial_perspective_lut_depth);
    let non_linear_w = sqrt(linear_w);
    let non_linear_slice = non_linear_w * aerial_perspective_lut_depth;

    var weight = 1.0;
    let half_slice_depth = 0.70710678118654752440084436210485f;  // sqrt(0.5f)
    if (non_linear_slice < half_slice_depth) {
        weight = saturate(non_linear_slice * non_linear_slice * 2.0);
    }

    let near_depth_fade_out = 1.0 / 0.001;
    weight *= saturate(relative_depth * near_depth_fade_out);

#if 0
    return f32x4(relative_depth, linear_slice, non_linear_w, linear_w);
#endif
    var aerial_perspective = aerial_perspective_lut.SampleLevel(sampler, f32x3(uv, non_linear_w), 0.0);
    aerial_perspective.xyz *= weight;
    aerial_perspective.w = 1.0 - (weight * (1.0 - aerial_perspective.w));
    aerial_perspective.xyz *= atmosphere.aerial_perspective_exposure;

    return aerial_perspective;
}
