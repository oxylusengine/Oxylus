import common;
import gpu;
import sky;
import pbr;
import scene;

typealias RWTextureCube<T:ITexelElement=float4, let sampleCount:int=0, let format:int=0> = _Texture<T, __ShapeCube, 0, 0, sampleCount, 1, 0, 0, format>;

struct ShaderParameters {
    Sampler sampler;
    Image2D<f32x4> sky_view_lut;
    Image2D<f32x4> sky_transmittance_lut;
    ConstantBuffer<Camera> camera;
    RWTextureCube<f32x4> mip_0;
    RWTextureCube<f32x4> mip_1;
    RWTextureCube<f32x4> mip_2;
    RWTextureCube<f32x4> mip_3;
    RWTextureCube<f32x4> mip_4;
    RWTextureCube<f32x4> mip_5;
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

func load_mip(ParameterBlock<ShaderParameters> params, u32 mip, i32x3 coord) -> f32x4 {
    switch (mip) {
        case 0: return params.mip_0.Load(coord);
        case 1: return params.mip_1.Load(coord);
        case 2: return params.mip_2.Load(coord);
        case 3: return params.mip_3.Load(coord);
        case 4: return params.mip_4.Load(coord);
        default: return params.mip_5.Load(coord);
    }
}

func store_mip(ParameterBlock<ShaderParameters> params, u32 mip, i32x3 coord, f32x4 value) -> void {
    switch (mip) {
        case 0: params.mip_0.Store(coord, value); return;
        case 1: params.mip_1.Store(coord, value); return;
        case 2: params.mip_2.Store(coord, value); return;
        case 3: params.mip_3.Store(coord, value); return;
        case 4: params.mip_4.Store(coord, value); return;
        default: params.mip_5.Store(coord, value); return;
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

func importance_sample_ggx(f32x2 xi, f32 alpha) -> f32x3 {
    let phi = TAU * xi.x;
    let alpha2 = alpha * alpha;
    let cos_theta = sqrt(saturate((1.0 - xi.y) / (1.0 + (alpha2 - 1.0) * xi.y)));
    let sin_theta = sqrt(saturate(1.0 - cos_theta * cos_theta));
    return f32x3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
}

func sample_sky_radiance(
    ParameterBlock<ShaderParameters> params,
    Atmosphere *atmosphere,
    f32x3 eye_pos,
    f32x3 dir,
    f32x3 sun_dir,
    f32x3 sun_illuminance
) -> f32x3 {
    var radiance = get_atmosphere_illuminance_along_ray(
        atmosphere,
        params.sampler,
        params.sky_view_lut,
        sun_dir,
        eye_pos,
        dir
    );

    let planet_intersection = com::ray_sphere_intersect_nearest(eye_pos, dir, atmosphere.planet_radius);
    if (!planet_intersection.hasValue) {
        radiance += draw_sun(dir, sun_dir, 1.0) * sun_illuminance;
    }

    return com::sanitize(radiance);
}

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform ParameterBlock<ShaderParameters> params,
    uniform Atmosphere *atmosphere,
    uniform f32x3 sun_dir,
    uniform f32 sun_intensity,
    uniform u32 frame_index,
    uniform u32 mip,
    uniform u32 mip_resolution,
    uniform f32 perceptual_roughness,
    uniform u32 sample_count,
) -> void {
    let face = thread_id.z;
    let pixel = thread_id.xy;
    if (any(pixel >= mip_resolution)) {
        return;
    }

    let uv = (f32x2(pixel) + 0.5) / f32(mip_resolution);
    let N = normalize(mul(CUBE_MAP_FACE_ROTATION(face), f32x3(uv * 2.0 - 1.0, -1.0)));

    let eye_pos = atmosphere.get_eye_pos(params.camera.position.xyz);

    let sun_cos_zenith = dot(sun_dir, f32x3(0.0, 1.0, 0.0));
    let transmittance_uv = transmittance_params_to_lut_uv(
        atmosphere.atmos_radius,
        atmosphere.planet_radius,
        f32x2(eye_pos.y, sun_cos_zenith));
    let sun_illuminance = params.sky_transmittance_lut.SampleLevel(params.sampler, transmittance_uv, 0.0).rgb
        * sun_intensity;

    let history = load_mip(params, mip, i32x3(pixel, face));
    let has_history = history.a > 0.0 && all(isfinite(history.rgb));

    let base_rotation = interleaved_gradient_noise(f32x2(pixel) + f32(face) * f32x2(37.0, 17.0) + f32(mip) * 91.0);
    let rotation = has_history
        ? frac(base_rotation + f32(frame_index) * 0.61803398875)
        : base_rotation;

    var frame_result = f32x3(0.0);
    let alpha = perceptual_roughness_to_alpha(perceptual_roughness);

    if (mip == 0) {
        frame_result = sample_sky_radiance(params, atmosphere, eye_pos, N, sun_dir, sun_illuminance);
    } else {
        let up = abs(N.y) < 0.999 ? f32x3(0.0, 1.0, 0.0) : f32x3(1.0, 0.0, 0.0);
        let tangent = normalize(cross(up, N));
        let bitangent = cross(N, tangent);

        var radiance_sum = f32x3(0.0);
        var weight_sum = 0.0;

        for (u32 i = 0; i < sample_count; i++) {
            let xi = f32x2((f32(i) + 0.5) / f32(sample_count), frac(radical_inverse_vdc(i) + rotation));
            let h_tangent = importance_sample_ggx(xi, alpha);
            let H = normalize(h_tangent.x * tangent + h_tangent.y * bitangent + h_tangent.z * N);
            let L = 2.0 * dot(N, H) * H - N;

            let NoL = dot(N, L);
            if (NoL <= 0.0) {
                continue;
            }

            radiance_sum += sample_sky_radiance(params, atmosphere, eye_pos, L, sun_dir, sun_illuminance) * NoL;
            weight_sum += NoL;
        }

        frame_result = weight_sum > 0.0
            ? radiance_sum / weight_sum
            : sample_sky_radiance(params, atmosphere, eye_pos, N, sun_dir, sun_illuminance);
    }

    frame_result = clamp(com::sanitize(frame_result), 0.0, com::HALF_MAX);

    let blend = mip == 0 ? 1.0 : 0.05;
    let accumulated = has_history ? lerp(history.rgb, frame_result, blend) : frame_result;

    store_mip(params, mip, i32x3(pixel, face), f32x4(accumulated, 1.0));
}
