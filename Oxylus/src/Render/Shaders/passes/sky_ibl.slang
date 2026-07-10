module sky_ibl;

import common;
import gpu;
import sky;
import scene;

#define IBL_CUBE_RES 32
#define IBL_SAMPLES_PER_FRAME 64

typealias RWTextureCube<T:ITexelElement=float4, let sampleCount:int=0, let format:int=0> = _Texture<T, __ShapeCube, 0, 0, sampleCount, 1, 0, 0, format>;

struct ShaderParameters {
    Sampler sampler;
    Image2D<f32x4> sky_view_lut;
    Image2D<f32x4> sky_transmittance_lut;
    ConstantBuffer<Camera> camera;
    RWTextureCube<f32x4> ibl_cube;
};

mat3 CUBE_MAP_FACE_ROTATION(uint face) {
    switch (face) {
        case 0: return mat3(+0, +0, -1, +0, -1, +0, -1, +0, +0);
        case 1: return mat3(+0, +0, +1, +0, -1, +0, +1, +0, +0);
        case 2: return mat3(+1, +0, +0, +0, +0, -1, +0, +1, +0);
        case 3: return mat3(+1, +0, +0, +0, +0, +1, +0, -1, +0);
        case 4: return mat3(+1, +0, +0, +0, -1, +0, +0, +0, -1);
        default: return mat3(-1, +0, +0, +0, -1, +0, +0, +0, +1);
    }
}

func radical_inverse_vdc(u32 bits) -> f32 {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return f32(bits) * 2.3283064365386963e-10;
}

func interleaved_gradient_noise(f32x2 p) -> f32 {
    let magic = f32x3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(p, magic.xy)));
}

func cosine_hemisphere_sample(u32 i, u32 sample_count, f32 rotation) -> f32x3 {
    let u1 = (f32(i) + 0.5) / f32(sample_count);
    let u2 = frac(radical_inverse_vdc(i) + rotation);
    let r = sqrt(u1);
    let phi = TAU * u2;
    return f32x3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
}

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform Atmosphere *atmosphere,
    uniform f32x3 sun_dir,
    uniform f32 sun_intensity,
    uniform f32 sun_ambient_strength,
    uniform u32 frame_index,
) -> void {
    let face = thread_id.z;
    let pixel = thread_id.xy;
    let uv = (f32x2(pixel) + 0.5) / f32(IBL_CUBE_RES);
    let output_dir = normalize(mul(CUBE_MAP_FACE_ROTATION(face), f32x3(uv * 2.0 - 1.0, -1.0)));

    var eye_altitude = params.camera.position.y * CAMERA_SCALE_UNIT;
    eye_altitude += atmosphere.planet_radius + PLANET_RADIUS_OFFSET;
    let eye_pos = f32x3(0.0, eye_altitude, 0.0);

    let up = abs(output_dir.y) < 0.999 ? f32x3(0.0, 1.0, 0.0) : f32x3(1.0, 0.0, 0.0);
    let tangent = normalize(cross(up, output_dir));
    let bitangent = cross(output_dir, tangent);

    let history = params.ibl_cube.Load(i32x3(pixel, face));
    let has_history = history.a > 0.0;

    let base_rotation = interleaved_gradient_noise(f32x2(pixel) + f32(face) * f32x2(37.0, 17.0));
    let rotation = has_history
        ? frac(base_rotation + f32(frame_index) * 0.61803398875)
        : base_rotation;

    f32x3 frame_sum = 0.0;
    for (u32 i = 0; i < IBL_SAMPLES_PER_FRAME; i++) {
        let local = cosine_hemisphere_sample(i, IBL_SAMPLES_PER_FRAME, rotation);
        let input_dir = normalize(local.x * tangent + local.y * bitangent + local.z * output_dir);
        let sample = get_atmosphere_illuminance_along_ray(
            atmosphere,
            params.sampler,
            params.sky_view_lut,
            sun_dir,
            eye_pos,
            input_dir
        );

        frame_sum += sample;
    }
    let frame_estimate = frame_sum / f32(IBL_SAMPLES_PER_FRAME);

    let sun_cos_zenith = dot(sun_dir, f32x3(0.0, 1.0, 0.0));
    let transmittance_uv = transmittance_params_to_lut_uv(
        atmosphere.atmos_radius,
        atmosphere.planet_radius,
        f32x2(eye_altitude, sun_cos_zenith));
    let sun_color = params.sky_transmittance_lut.SampleLevel(params.sampler, transmittance_uv, 0.0).rgb;
    let sun_facing = saturate(dot(output_dir, sun_dir));
    let sun_term = sun_ambient_strength * sun_color * sun_intensity * sun_facing / PI;

    let frame_result = frame_estimate + sun_term;

    let alpha = 0.02;
    let accumulated = has_history ? lerp(history.rgb, frame_result, alpha) : frame_result;

    params.ibl_cube.Store(i32x3(pixel, face), f32x4(accumulated, 1.0));
}
