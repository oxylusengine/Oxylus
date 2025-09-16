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
    Image2D<f32> gtao_image;
    Image2D<f32> directional_shadows_map;
    ConstantBuffer<Scene> scene;
    ConstantBuffer<Camera> camera;
    ConstantBuffer<Lights> lights;
};

func lights_attenuate_point(f32 to_light_dist, f32 cutoff) -> f32 {
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

    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic);

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

    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic);

    f32x3 radiance = light.color * total_attenuation * light.intensity;

    return brdf * radiance * NdotL;
}

static const f32x2 POISSON_DISK[16] = {
    f32x2(-0.94201624, -0.39906216),
    f32x2(0.94558609, -0.76890725),
    f32x2(-0.094184101, -0.92938870),
    f32x2(0.34495938, 0.29387760),
    f32x2(-0.91588581, 0.45771432),
    f32x2(-0.81544232, -0.87912464),
    f32x2(-0.38277543, 0.27676845),
    f32x2(0.97484398, 0.75648379),
    f32x2(0.44323325, -0.97511554),
    f32x2(0.53742981, -0.47373420),
    f32x2(-0.26496911, -0.41893023),
    f32x2(0.79197514, 0.19090188),
    f32x2(-0.24188840, 0.99706507),
    f32x2(-0.81409955, 0.91437590),
    f32x2(0.19984126, 0.78641367),
    f32x2(0.14383161, -0.14100790)
};

f32 poisson_disk_shadow(Image2D<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord, f32 bias, f32 filter_radius) {
    f32 shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        f32x2 offset = POISSON_DISK[i] * filter_radius;
        f32 shadow_depth = shadow_map.sample_mip(shadow_sampler, shadow_coord.xy + offset, 0).r;
        shadow += (shadow_coord.z + bias) < shadow_depth ? 0.0 : 1.0;
    }
    return shadow / 16.0;
}

f32 pcf_shadow(Image2D<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord, f32 bias, f32 texel_size) {
    f32 shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            f32x2 offset = f32x2(x, y) * texel_size;
            f32 shadow_depth = shadow_map.sample_mip(shadow_sampler, shadow_coord.xy + offset, 0).r;
            shadow += (shadow_coord.z + bias) < shadow_depth ? 0.0 : 1.0;
        }
    }
    return shadow / 9.0;
}

f32 find_blocker_distance(Image2D<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord, f32 light_size, f32 bias) {
    f32 search_radius = light_size * (shadow_coord.z - 0.1) / shadow_coord.z;
    f32 blocker_sum = 0.0;
    f32 num_blockers = 0.0;

    for (int i = 0; i < 16; ++i) {
        f32x2 offset = POISSON_DISK[i] * search_radius;
        f32 shadow_depth = shadow_map.sample_mip(shadow_sampler, shadow_coord.xy + offset, 0).r;

        if ((shadow_coord.z + bias) > shadow_depth) {
            blocker_sum += shadow_depth;
            num_blockers += 1.0;
        }
    }

    if (num_blockers == 0.0) return -1.0;
    return blocker_sum / num_blockers;
}

f32 contact_hardening_shadow(Image2D<f32> shadow_map, Sampler shadow_sampler, f32x3 shadow_coord,
                           f32 light_size, f32 bias, f32x3 world_pos, f32x3 light_pos) {
    f32 light_distance = length(light_pos - world_pos);

    f32 avg_blocker_depth = find_blocker_distance(shadow_map, shadow_sampler, shadow_coord, light_size * 0.1, bias);

    if (avg_blocker_depth < 0.0) return 0.0;

    f32 blocker_world_distance = avg_blocker_depth * light_distance;
    f32 receiver_world_distance = shadow_coord.z * light_distance;

    f32 penumbra_size = light_size * (receiver_world_distance - blocker_world_distance) / blocker_world_distance;
    f32 filter_radius = penumbra_size / light_distance;

    filter_radius = clamp(filter_radius, 0.001, 0.05);

    return poisson_disk_shadow(shadow_map, shadow_sampler, shadow_coord, bias, filter_radius);
}

u32 get_cascade_index(f32 view_depth) {
    return 0;
}

f32 sample_shadow_map(Image2D<f32> shadow_map, Sampler shadow_sampler, f32x4x4 light_view_projection[4],
                         f32x3 world_position, f32x3 normal, f32x3 light_direction, f32x3 camera_position) {
    f32 view_depth = length(world_position - camera_position);
    u32 cascade_index = get_cascade_index(view_depth);

    f32x4x4 projection = light_view_projection[cascade_index];
    f32x4 shadow_coord = mul(projection, f32x4(world_position, 1.0));

    shadow_coord.xyz /= shadow_coord.w;
    shadow_coord.xy = shadow_coord.xy * 0.5 + 0.5;

    if (shadow_coord.x < 0.0 || shadow_coord.x > 1.0 ||
        shadow_coord.y < 0.0 || shadow_coord.y > 1.0) {
        return 0.0;
    }

    f32 bias = max(0.002 * (1.0 - dot(normal, -light_direction)), 0.0005);

    //return pcf_shadow(shadow_map, shadow_sampler, shadow_coord.xyz, bias, 0.002);
    //return poisson_disk_shadow(shadow_map, shadow_sampler, shadow_coord.xyz, bias, 0.002);
    return contact_hardening_shadow(shadow_map, shadow_sampler, shadow_coord.xyz, 0.1, bias, world_position, 1000);
}

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform ParameterBlock<ShaderParameters> params, uniform f32x4x4 directional_light_projection) -> f32x4 {
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

    f32x4x4 directional_light_projections[4] = {};
    directional_light_projections[0] = directional_light_projection;
    f32 directional_shadows = sample_shadow_map(params.directional_shadows_map,
                                params.linear_clamp_sampler,
                                directional_light_projections,
                                world_position,
                                N, directional_light.direction, params.camera.position.xyz);

    f32x3 diffuse_light = f32x3(0, 0, 0);

    // SUN LIGHT COLOR ──────────────────────────────────────────────────
    const f32x3 ray_pos_planet = f32x3(0.0, world_position.y, 0.0) * CAMERA_SCALE_UNIT + f32x3(0.0, params.scene.atmosphere.planet_radius, 0.0);
    f32 h = length(ray_pos_planet);
    f32x3 up_vec = normalize(ray_pos_planet);
    f32 sun_cos_theta = dot(L, up_vec);
    f32x2 transmittance_uv = transmittance_params_to_lut_uv(params.scene.atmosphere, f32x2(h, sun_cos_theta));
    f32x3 sun_transmittance = params.sky_transmittance_lut.sample_mip(params.linear_clamp_sampler, transmittance_uv, 0.0).rgb;
    diffuse_light += sun_transmittance * directional_light.intensity * directional_shadows;

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
            light
        );

        total_lighting += spot_light_contribution;
    }


    // SKY AMBIENT COLOR ────────────────────────────────────────────────
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

    f32 eye_gradient = dot(N, sky_info.eye_dir);
    eye_gradient = (eye_gradient + 1.0) * 0.375 + 0.25;
    f32x3 ambient_contribution = com::rec709_oetf(sky_result.luminance) * albedo_color * eye_gradient * occlusion;

    // MATERIAL COLOR (Sun Light) ──────────────────────────────────────
    // https://marmosetco.tumblr.com/post/81245981087
    const f32x3 R = reflect(-V, N);
    const f32 horizon_fade = 1.3;
    f32 horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    const f32 NoL = max(dot(N, L), 0.0);
    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic);
    f32x3 material_surface_color = brdf * horizon * diffuse_light * NoL * occlusion;

    // FINAL ────────────────────────────────────────────────────────────
    f32x3 final_color = material_surface_color + total_lighting + ambient_contribution + emission;

    return f32x4(final_color, 1.0);
}