module sky_multiscattering;

import common;
import gpu;
import sky;
import scene;

import embedded.hemisphere;

[[vk::binding(0, 0)]]
Sampler sampler;
[[vk::binding(1, 0)]]
Image2D<f32x4> sky_transmittance_lut;
[[vk::binding(2, 0)]]
StorageImage2D<f32x4, ImageFormat::RGBA16F> sky_multiscattering_lut;

struct PushConstants {
    Atmosphere *atmosphere;
};
[[vk::push_constant]] PushConstants C;

#define SAMPLE_COUNT 64

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) {
    f32x2 uv = f32x2(f32x2(thread_id.xy) + 0.5) / f32x2(C.atmosphere->multiscattering_lut_size.xy);

    const f32 atmosphere_thickness = C.atmosphere->atmos_radius - C.atmosphere.planet_radius;
    const f32 altitude = C.atmosphere->planet_radius + uv.y * atmosphere_thickness + PLANET_RADIUS_OFFSET;

    f32 sun_cos_theta = uv.x * 2.0 - 1.0;
    f32x3 sun_dir = f32x3(0.0, sun_cos_theta, com::safe_sqrt(1.0 - sun_cos_theta * sun_cos_theta));
    f32x3 ray_pos = f32x3(0.0, altitude, 0.0);

    AtmosphereIntegrateInfo info = {};
    info.eye_pos = ray_pos;
    info.sun_dir = sun_dir;
    info.eval_mie_phase = false;
    info.eval_rayleigh_phase = false;
    info.eval_planet_luminance = true;
    info.sampling.variable_sample_count = false;
    info.sampling.initial_sample_count = 32;

    info.transmittance_image = sky_transmittance_lut;

    f32x3 luminance = 0.0;
    f32x3 multi_scattering_as_1 = 0.0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        info.eye_dir = HEMISPHERE_64[i];
        const let result = integrate_single_scattered_luminance(C.atmosphere, sampler, info);
        multi_scattering_as_1 += result.multiscattering_as_1;
        luminance += result.luminance;
    }

    const let sphere_solid_angle = 4.0f * PI;
    const let isotropic_phase = 1.0f / sphere_solid_angle;
    const let inv_sample_count = 1.0 / f32(SAMPLE_COUNT);
    luminance *= sphere_solid_angle * inv_sample_count;
    multi_scattering_as_1 *= inv_sample_count;
    f32x3 scattered_luminance = luminance * isotropic_phase;
    f32x3 f_ms = 1.0 / (1.0 - multi_scattering_as_1);

    sky_multiscattering_lut.store(thread_id.xy, f32x4(scattered_luminance * f_ms, 1.0));
}

