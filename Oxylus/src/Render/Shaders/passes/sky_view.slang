import common;
import gpu;
import sky;
import scene;

struct ShaderParameters {
    Sampler sampler;
    Image2D<f32x4> sky_transmittance_lut;
    Image2D<f32x4> sky_multiscattering_lut;
    ConstantBuffer<Atmosphere> atmosphere;
    StorageImage2D<f32x4, ImageFormat::RGBA16F> sky_view_lut;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform f32x3 sun_dir,
    uniform f32 sun_intensity,
    uniform f32x3 camera_pos,
) {
    let uv = (f32x2(thread_id.xy) + 0.5) / f32x2(params.atmosphere.sky_view_lut_size.xy);
    var eye_pos = params.atmosphere.get_eye_pos(camera_pos);

    let h = length(eye_pos);
    let eye_dir = uv_to_sky_view_lut_params(params.atmosphere, uv, h);

    if (!move_to_top_atmosphere(eye_pos, eye_dir, params.atmosphere.atmos_radius)) {
        params.sky_view_lut.Store(thread_id.xy, 0.0);
        return;
    }

    let up_vec = eye_pos / h;
    let sun_zenith_cos_angle = dot(sun_dir, up_vec);
    let sun_dir = normalize(f32x3(com::safe_sqrt(1.0 - sun_zenith_cos_angle * sun_zenith_cos_angle), sun_zenith_cos_angle, 0.0));

    AtmosphereIntegrateInfo info = {};
    info.eye_pos = eye_pos;
    info.eye_dir = eye_dir;
    info.sun_dir = sun_dir;
    info.sun_intensity = sun_intensity;
    info.step_count = 48;

    let result = integrate_single_scattered_luminance(info, params.atmosphere, params.sampler, params.sky_transmittance_lut, params.sky_multiscattering_lut);

    let luminance = com::sanitize(max(result.luminance, 0.0));
    let max_luminance = max(luminance.x, max(luminance.y, luminance.z));
    let mult = clamp(max_luminance, com::HALF_MIN_NORMAL, com::HALF_MAX);

    params.sky_view_lut.Store(thread_id.xy, f32x4(luminance / mult, mult));
}
