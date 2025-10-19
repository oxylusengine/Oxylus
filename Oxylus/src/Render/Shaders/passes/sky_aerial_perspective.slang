module sky_aerial_perspective;

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
    ConstantBuffer<Camera> camera;
    StorageImage3D<f32x4, ImageFormat::RGBA16F> sky_aerial_perspective_lut;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params
) -> void {
    const let lut_size = params.atmosphere.aerial_perspective_lut_size;
    f32x2 uv = f32x2(f32x2(thread_id.xy) + 0.5) / f32x2(lut_size.xy);
    f32x3 clip_space = f32x3(2.0 * uv - 1.0, 1.0);
    f32x4 world_space = mul(params.camera.inv_projection_view, f32x4(clip_space, 1.0));
    f32x3 world_pos = world_space.xyz / world_space.w;
    f32x3 world_dir = normalize(world_pos - params.camera.position.xyz);
    f32x3 eye_pos = params.atmosphere.eye_pos;

    f32 slice = ((f32(thread_id.z) + 0.5) * (1.0 / lut_size.z));
    slice *= slice;
    slice *= lut_size.z;

    const i32 step_count = int(max(1.0, f32(thread_id.z + 1.0) * 2.0));
    const f32 per_slice_depth = f32(lut_size.x / lut_size.z);
    const f32 start_depth = params.atmosphere.aerial_perspective_start_km * INV_CAMERA_SCALE_UNIT;
    f32 t_max = slice * per_slice_depth;

    f32x3 start_pos = eye_pos + start_depth * world_dir;
    f32x3 ray_pos = start_pos + t_max * world_dir;
    f32 view_height = length(ray_pos);

    // if (view_height <= (params.atmosphere.planet_radius + PLANET_RADIUS_OFFSET)) {
    //     ray_pos = normalize(ray_pos) * (params.atmosphere.planet_radius + PLANET_RADIUS_OFFSET);
    //     world_dir = normalize(ray_pos - eye_pos);
    //     t_max = length(ray_pos - eye_pos);
    // }

    f32 t_max_max = t_max;
    view_height = length(eye_pos);
    if (view_height >= params.atmosphere.atmos_radius) {
        f32x3 prev_ray_pos = eye_pos;
        if (!move_to_top_atmosphere(eye_pos, world_dir, params.atmosphere.atmos_radius)) {
            params.sky_aerial_perspective_lut.store(thread_id, 0.0);
            return;
        }
        f32 length_to_atmosphere = length(prev_ray_pos - eye_pos);
        if (t_max_max < length_to_atmosphere) {
            params.sky_aerial_perspective_lut.store(thread_id, 0.0);
            return;
        }

        t_max_max = max(0.0, t_max_max - length_to_atmosphere);
     }

    AtmosphereIntegrateInfo info = {};
    info.eye_pos = eye_pos;
    info.eye_dir = world_dir;
    info.sun_dir = normalize(params.directional_light.direction);
    info.sun_intensity = params.directional_light.intensity;
    info.eval_planet_luminance = false;
    info.step_count = max(1.0, (f32(thread_id.z) + 1.0) * 2.0);
    let result = integrate_single_scattered_luminance(
            info, params.atmosphere, params.sampler, params.sky_transmittance_lut, params.sky_multiscattering_lut);
    let inv_luminance = 1.0 / max(result.luminance, float3(1.0 / 1048576.0));
    let inv_mult = min(1048576.0, max(inv_luminance.x, max(inv_luminance.y, inv_luminance.z)));
    params.sky_aerial_perspective_lut.store(thread_id, f32x4(result.luminance, inv_mult));
}
