module brdf;

import gpu;
import common;
import pbr;
import sky;
import scene;

#include <fullscreen.slang>

struct ShaderParameters {
    Sampler linear_clamp_sampler;
    Sampler linear_repeat_sampler;
    Image2D<f32x4> sky_transmittance_lut;
    Image2D<f32x4> sky_multiscattering_lut;
    Image2D<f32> depth_image;
    Image2D<f32x4> albedo_image;
    Image2D<f32x4> normal_image;
    Image2D<f32x3> emissive_image;
    Image2D<f32x3> metallic_roughness_occlusion_image;
    ConstantBuffer<Atmosphere> atmosphere;
    ConstantBuffer<Sun> sun;
    ConstantBuffer<Camera> camera;
};

ParameterBlock<ShaderParameters> params;

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    const u32x2 pixel_pos = u32x2(input.position.xy);
    const f32 depth = params.depth_image.load(pixel_pos);
    if (depth == 1.0) {
        discard;
    }

    const f32x3 albedo_color = params.albedo_image.sample_mip(params.linear_repeat_sampler, input.tex_coord, 0).rgb;

    const f32x4 mapped_smooth_normal = params.normal_image.load(pixel_pos);
    const f32x3 mapped_normal = com::oct_to_vec3(mapped_smooth_normal.xy);
    const f32x3 smooth_normal = com::oct_to_vec3(mapped_smooth_normal.zw);

    const f32x3 emission = params.emissive_image.load(pixel_pos);

    const f32x3 metallic_roughness_occlusion = params.metallic_roughness_occlusion_image.load(pixel_pos);
    const f32 metallic = metallic_roughness_occlusion.x;
    const f32 roughness = metallic_roughness_occlusion.y;
    const f32 occlusion = metallic_roughness_occlusion.z;

    const f32x3 NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    const f32x4 world_position_h = mul(params.camera.inv_projection_view, f32x4(NDC, 1.0));
    const f32x3 world_position = world_position_h.xyz / world_position_h.w;

    // PBR constants
    const f32x3 V = normalize(params.camera.position.xyz - world_position);
    const f32x3 L = normalize(params.sun.direction); // temp
    const f32x3 N = mapped_normal;

    // SUN LIGHT COLOR ──────────────────────────────────────────────────
    const f32x3 ray_pos_planet = f32x3(0.0, world_position.y, 0.0) * CAMERA_SCALE_UNIT + f32x3(0.0, params.atmosphere.planet_radius, 0.0);
    f32 h = length(ray_pos_planet);
    f32x3 up_vec = normalize(ray_pos_planet);
    f32 sun_cos_theta = dot(L, up_vec);
    f32x2 transmittance_uv = transmittance_params_to_lut_uv(params.atmosphere, f32x2(h, sun_cos_theta));
    f32x3 sun_transmittance = params.sky_transmittance_lut.sample_mip(params.linear_clamp_sampler, transmittance_uv, 0.0).rgb;
    f32x3 sun_illuminance = sun_transmittance * params.sun.intensity;

    // SKY AMBIENT COLOR ────────────────────────────────────────────────
    AtmosphereIntegrateInfo sky_info = {};
    sky_info.eye_pos = ray_pos_planet;
    sky_info.sun_dir = L;
    sky_info.sun_intensity = params.sun.intensity;
    sky_info.sampling.variable_sample_count = true;
    sky_info.sampling.min_sample_count = 1;
    sky_info.sampling.max_sample_count = 4;
    sky_info.transmittance_image = params.sky_transmittance_lut;
    sky_info.multiscattering_image = params.sky_multiscattering_lut;
    sky_info.eval_mie_phase = false;

    sky_info.eye_dir = f32x3(0.0, 1.0, 0.0);
    const let sky_result = integrate_single_scattered_luminance(params.atmosphere, params.linear_clamp_sampler, sky_info);

    f32 eye_gradient = dot(N, sky_info.eye_dir);
    eye_gradient = (eye_gradient + 1.0) * 0.375 + 0.25;
    f32x3 ambient_contribution = com::rec709_oetf(sky_result.luminance) * albedo_color * occlusion * eye_gradient;

    // MATERIAL COLOR ───────────────────────────────────────────────────
    // https://marmosetco.tumblr.com/post/81245981087
    const f32x3 R = reflect(-V, N);
    const f32 horizon_fade = 1.3;
    f32 horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    const f32 NoL = max(dot(N, L), 0.0);
    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic);
    f32x3 material_surface_color = brdf * horizon  * sun_illuminance * NoL;

    // FINAL ────────────────────────────────────────────────────────────
    f32x3 final_color = material_surface_color + ambient_contribution + emission;

    return f32x4(final_color, 1.0);
}
