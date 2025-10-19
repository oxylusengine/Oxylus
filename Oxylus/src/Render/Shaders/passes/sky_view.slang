module sky_view;

import common;
import gpu;
import sky;
import scene;

struct ShaderParameters {
    Sampler sampler;
    Image2D<f32x4> sky_transmittance_lut;
    Image2D<f32x4> sky_multiscattering_lut;
    ConstantBuffer<Atmosphere> atmosphere;
    ConstantBuffer<DirectionalLight> directional_light;
    StorageImage2D<f32x4, ImageFormat::RGBA16F> sky_view_lut;
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
    f32 sun_zenith_cos_angle = dot(normalize(params.directional_light.direction), up_vec);
    f32x3 sun_dir = normalize(f32x3(com::safe_sqrt(1.0 - sun_zenith_cos_angle * sun_zenith_cos_angle), sun_zenith_cos_angle, 0.0));

    AtmosphereIntegrateInfo info = {};
    info.eye_pos = eye_pos;
    info.eye_dir = eye_dir;
    info.sun_dir = sun_dir;
    info.sun_intensity = params.directional_light.intensity;
    info.step_count = 48;

    let result = integrate_single_scattered_luminance(info, params.atmosphere, params.sampler, params.sky_transmittance_lut, params.sky_multiscattering_lut);
    let inv_luminance = 1.0 / max(result.luminance, float3(1.0 / 1048576.0));
    let inv_mult = min(1048576.0, max(inv_luminance.x, max(inv_luminance.y, inv_luminance.z)));

    params.sky_view_lut.store(thread_id.xy, f32x4(result.luminance, inv_mult));
}
