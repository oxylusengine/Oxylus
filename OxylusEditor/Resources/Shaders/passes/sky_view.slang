module sky_view;

import common;
import gpu;
import sky;
import scene;

struct ShaderParameters {
    Image2D<f32x4> sky_transmittance_lut;
    Image2D<f32x4> sky_multiscattering_lut;
    StorageImage2D<f32x4, ImageFormat::RGBA16F> sky_view_lut;
    ConstantBuffer<Atmosphere> atmosphere;
    ConstantBuffer<Sun> sun;
    ConstantBuffer<Camera> camera;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params
) {
    f32x2 uv = f32x2(thread_id.xy) / f32x2(params.atmosphere.sky_view_lut_size.xy);
    f32x3 eye_pos = params.atmosphere.eye_pos;

    f32 h = length(eye_pos);
    const f32x3 eye_dir = uv_to_sky_view_lut_params(params.atmosphere, uv, h);

    if (!move_to_top_atmosphere(eye_pos, eye_dir, params.atmosphere.atmos_radius)) {
        params.sky_view_lut.store(thread_id.xy, 0.0);
        return;
    }

    f32x3 up_vec = eye_pos / h;
    f32 sun_zenith_cos_angle = dot(normalize(params.sun.direction), up_vec);
    f32x3 sun_dir = normalize(f32x3(com::safe_sqrt(1.0 - sun_zenith_cos_angle * sun_zenith_cos_angle), sun_zenith_cos_angle, 0.0));

    AtmosphereIntegrateInfo info = {};
    info.eye_pos = eye_pos;
    info.eye_dir = eye_dir;
    info.sun_dir = sun_dir;
    info.sun_intensity = params.sun.intensity;

    const int sample_count = 48;
    info.sampling.variable_sample_count = true;
    info.sampling.min_sample_count = sample_count;
    info.sampling.max_sample_count = sample_count;

    info.transmittance_image = params.sky_transmittance_lut;
    info.multiscattering_image = params.sky_multiscattering_lut;

    const let result = integrate_single_scattered_luminance(params.atmosphere, material_samplers[1], info);
    const let transmittance = dot(result.transmittance, 1.0 / 3.0);

    params.sky_view_lut.store(thread_id.xy, f32x4(result.luminance, transmittance));
}
