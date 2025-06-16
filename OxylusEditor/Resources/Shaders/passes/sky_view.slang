module sky_view;

import common;
import gpu;
import sky;
import scene;

[[vk::binding(0, 0)]]
Image2D<f32x4> sky_transmittance_lut;
[[vk::binding(1, 0)]]
Image2D<f32x4> sky_multiscattering_lut;
[[vk::binding(2, 0)]]
StorageImage2D<f32x4, ImageFormat::RGBA16F> sky_view_lut;

struct PushConstants {
    Atmosphere *atmosphere;
    Sun *sun;
    Camera* camera_buffer;
};
[[vk::push_constant]] PushConstants C;

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) {
    f32x2 uv = f32x2(thread_id.xy) / f32x2(C.atmosphere->sky_view_lut_size.xy);
    f32x3 eye_pos = C.atmosphere->eye_pos;

    f32 h = length(eye_pos);
    const f32x3 eye_dir = uv_to_sky_view_lut_params(C.atmosphere, uv, h);

    if (!move_to_top_atmosphere(eye_pos, eye_dir, C.atmosphere->atmos_radius)) {
        sky_view_lut.store(thread_id.xy, 0.0);
        return;
    }

    f32x3 up_vec = eye_pos / h;
    f32 sun_zenith_cos_angle = dot(normalize(C.sun->direction), up_vec);
    f32x3 sun_dir = normalize(f32x3(com::safe_sqrt(1.0 - sun_zenith_cos_angle * sun_zenith_cos_angle), sun_zenith_cos_angle, 0.0));

    AtmosphereIntegrateInfo info = {};
    info.eye_pos = eye_pos;
    info.eye_dir = eye_dir;
    info.sun_dir = sun_dir;
    info.sun_intensity = C.sun->intensity;

    const int sample_count = 48;
    info.sampling.variable_sample_count = true;
    info.sampling.min_sample_count = sample_count;
    info.sampling.max_sample_count = sample_count;

    info.transmittance_image = sky_transmittance_lut;
    info.multiscattering_image = sky_multiscattering_lut;

    const let result = integrate_single_scattered_luminance(C.atmosphere, material_samplers[1], info);
    const let transmittance = dot(result.transmittance, 1.0 / 3.0);

    sky_view_lut.store(thread_id.xy, f32x4(result.luminance, transmittance));
}
