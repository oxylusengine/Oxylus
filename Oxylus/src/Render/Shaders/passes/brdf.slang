module brdf;

import gpu;
import common;
import pbr;
import sky;
import scene;
import shadows;

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
    Image2D<f32> gtao_image;
    Image2D<f32> contact_shadows;
    Texture2DArray<f32> directional_shadows_map;
    ConstantBuffer<Scene> scene;
    ConstantBuffer<Camera> camera;
    ConstantBuffer<Lights> lights;
};

func lights_attenuate_point(f32 to_light_dist, f32 cutoff) -> f32 {
    // infinite cutoff
    if (cutoff <= 0.0) {
        return 1.0 / (to_light_dist * to_light_dist + 0.1f);
    }

    f32 win = (to_light_dist / cutoff);
    win = win * win * win * win;
    win = max(0.0, 1.0 - win);
    win = win * win;
    f32 attenuation = win / (to_light_dist * to_light_dist + 0.1);
    return attenuation;
}

f32x3 point_light_contribution(
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 albedo_color,
    f32 roughness,
    f32 metallic,
    f32x3 ambient_light_occluded,
    PointLight light
) {
    f32x3 light_vector = light.position - world_position;
    f32 light_distance = length(light_vector);
    f32x3 L = light_vector / light_distance;

    f32 attenuation = lights_attenuate_point(light_distance, light.cutoff);

    if (attenuation <= 0.0 || light.intensity <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    f32 NdotL = saturate(dot(N, L));

    if (NdotL <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic, ambient_light_occluded);

    f32x3 radiance = light.color * attenuation * light.intensity;

    return brdf * radiance * NdotL;
}

f32 calculate_spot_light_attenuation(f32x3 light_direction, f32x3 spot_direction, f32 inner_cone_angle, f32 outer_cone_angle) {
    f32 cos_angle = dot(-light_direction, normalize(spot_direction));

    f32 cos_inner = cos(radians(inner_cone_angle * 0.5));
    f32 cos_outer = cos(radians(outer_cone_angle * 0.5));

    f32 spot_attenuation = smoothstep(cos_outer, cos_inner, cos_angle);

    return spot_attenuation;
}

f32x3 calculate_spot_light_contribution(
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 albedo_color,
    f32 roughness,
    f32 metallic,
    f32x3 ambient_light_occluded,
    SpotLight light
) {
    f32x3 light_vector = light.position - world_position;
    f32 light_distance = length(light_vector);
    f32x3 L = light_vector / light_distance;

    f32 distance_attenuation = lights_attenuate_point(light_distance, light.cutoff);

    f32 spot_attenuation = calculate_spot_light_attenuation(L, light.direction, light.inner_cone_angle, light.outer_cone_angle);

    f32 total_attenuation = distance_attenuation * spot_attenuation;

    if (total_attenuation <= 0.0 || light.intensity <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    f32 NdotL = saturate(dot(N, L));

    if (NdotL <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic, ambient_light_occluded);

    f32x3 radiance = light.color * total_attenuation * light.intensity;

    return brdf * radiance * NdotL;
}

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params) -> f32x4 {
    const u32x2 pixel_pos = u32x2(input.position.xy);
    const f32 depth = params.depth_image.load(pixel_pos);
    if (depth == 0.0) {
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
    f32 occlusion = metallic_roughness_occlusion.z;

    bool gtao_enabled = params.scene.scene_flags & SceneFlags::HasGTAO;
    f32 screen_space_occlusion = 1.0;
    if (gtao_enabled) {
        screen_space_occlusion = params.gtao_image.load(pixel_pos).r;
    }

    occlusion = min(occlusion, screen_space_occlusion);

    const f32x3 NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    const f32x4 world_position_h = mul(params.camera.inv_projection_view, f32x4(NDC, 1.0));
    const f32x3 world_position = world_position_h.xyz / world_position_h.w;

    let directional_light = params.lights.directional_light;

    // PBR constants
    const f32x3 V = normalize(params.camera.position.xyz - world_position);
    const f32x3 L = normalize(directional_light.direction);
    const f32x3 N = mapped_normal;

    let view_z = -mul(params.camera.view, f32x4(world_position, 1.0)).z;
    f32 directional_shadows = sample_shadow_map(
        params.lights.directional_light,
        params.directional_shadows_map,
        params.linear_clamp_sampler,
        view_z,
        world_position,
        N, directional_light.direction);

    let cascade_index = get_cascade_index(params.lights.directional_light, view_z);

    f32x3 diffuse_light = f32x3(0, 0, 0);

    // SKY AMBIENT COLOR ────────────────────────────────────────────────
    const f32x3 ray_pos_planet = f32x3(0.0, world_position.y, 0.0) * CAMERA_SCALE_UNIT + f32x3(0.0, params.scene.atmosphere.planet_radius, 0.0);

    AtmosphereIntegrateInfo sky_info = {};
    sky_info.eye_pos = ray_pos_planet;
    sky_info.sun_dir = L;
    sky_info.sun_intensity = directional_light.intensity;
    sky_info.sampling.variable_sample_count = true;
    sky_info.sampling.min_sample_count = 1;
    sky_info.sampling.max_sample_count = 4;
    sky_info.transmittance_image = params.sky_transmittance_lut;
    sky_info.multiscattering_image = params.sky_multiscattering_lut;
    sky_info.eval_mie_phase = false;

    sky_info.eye_dir = f32x3(0.0, 1.0, 0.0);
    const let sky_result = integrate_single_scattered_luminance(params.scene.atmosphere, params.linear_clamp_sampler, sky_info);

    f32x3 ambient_light = com::rec709_oetf(sky_result.luminance);
    f32x3 ambient_light_occluded = ambient_light * occlusion;
    f32 eye_gradient = dot(N, sky_info.eye_dir);
    eye_gradient = (eye_gradient + 1.0) * 0.375 + 0.25;

    f32 contact_shadows = 1.0f;
    bool contact_shadows_enabled = params.scene.scene_flags & SceneFlags::HasContactShadows;
    if (contact_shadows_enabled) {
        contact_shadows = 1.0f - params.contact_shadows.load(pixel_pos).r;
    }

    // SUN LIGHT COLOR ──────────────────────────────────────────────────
    f32 h = length(ray_pos_planet);
    f32x3 up_vec = normalize(ray_pos_planet);
    f32 sun_cos_theta = dot(L, up_vec);
    f32x2 transmittance_uv = transmittance_params_to_lut_uv(params.scene.atmosphere, f32x2(h, sun_cos_theta));
    f32x3 sun_transmittance = params.sky_transmittance_lut.sample_mip(params.linear_clamp_sampler, transmittance_uv, 0.0).rgb;
    diffuse_light += sun_transmittance * directional_light.intensity * directional_shadows * contact_shadows;

    f32x3 total_lighting = f32x3(0.0, 0.0, 0.0);

    for(int point_light_idx = 0; point_light_idx < params.lights.point_light_count; point_light_idx++) {
        PointLight light = params.lights.point_lights[point_light_idx];

        f32x3 point_light_contribution = point_light_contribution(
            world_position,
            N,
            V,
            albedo_color,
            roughness,
            metallic,
            ambient_light_occluded,
            light
        );

        total_lighting += point_light_contribution;
    }

    for(int spot_light_idx = 0; spot_light_idx < params.lights.spot_light_count; spot_light_idx++) {
        SpotLight light = params.lights.spot_lights[spot_light_idx];

        f32x3 spot_light_contribution = calculate_spot_light_contribution(
            world_position,
            N,
            V,
            albedo_color,
            roughness,
            metallic,
            ambient_light_occluded,
            light
        );

        total_lighting += spot_light_contribution;
    }

    f32x3 ambient_contribution = ambient_light_occluded * albedo_color * eye_gradient;

    // MATERIAL COLOR (Sun Light) ──────────────────────────────────────
    // https://marmosetco.tumblr.com/post/81245981087
    const f32x3 R = reflect(-V, N);
    const f32 horizon_fade = 1.3;
    f32 horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    const f32 NoL = max(dot(N, L), 0.0);
    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic, ambient_light_occluded);
    f32x3 material_surface_color = brdf * horizon * diffuse_light * NoL * occlusion;

    // FINAL ────────────────────────────────────────────────────────────
    f32x3 final_color = material_surface_color + total_lighting + ambient_contribution + emission;

    return f32x4(final_color, 1.0);
}
