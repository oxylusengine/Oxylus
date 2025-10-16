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
    Texture2D<f32x4> sky_transmittance_lut;
    Texture2D<f32x4> sky_multiscattering_lut;
    Texture2D<f32> depth_image;
    Texture2D<f32x4> albedo_image;
    Texture2D<f32x4> normal_image;
    Texture2D<f32x3> emissive_image;
    Texture2D<f32x3> metallic_roughness_occlusion_image;
    Texture2D<f32> gtao_image;
    Texture2D<f32> contact_shadows;
    Texture2DArray<f32> directional_shadows_map;
    ConstantBuffer<Lights> lights;
    ConstantBuffer<Camera> camera;
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
    f32x3 ambient_light,
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

    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic, ambient_light);

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
    f32x3 ambient_light,
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

    f32x3 brdf = BRDF(V, N, L, albedo_color, roughness, metallic, ambient_light);

    f32x3 radiance = light.color * total_attenuation * light.intensity;

    return brdf * radiance * NdotL;
}

[[shader("fragment")]]
func fs_main(
    VertexOutput input,
    uniform ParameterBlock<ShaderParameters> params,
    uniform SceneFlags scene_flags
) -> f32x4 {
    let pixel_pos = u32x3(u32x2(input.position.xy), 0);
    let depth = params.depth_image.Load(pixel_pos);
    if (depth == 0.0) {
        discard;
    }

    let albedo_color = params.albedo_image.SampleLevel(params.linear_repeat_sampler, input.tex_coord, 0).rgb;

    let mapped_smooth_normal = params.normal_image.Load(pixel_pos);
    let mapped_normal = com::oct_to_vec3(mapped_smooth_normal.rg);
    let smooth_normal = com::oct_to_vec3(mapped_smooth_normal.ba);

    let emission = params.emissive_image.Load(pixel_pos);

    let metallic_roughness_occlusion = params.metallic_roughness_occlusion_image.Load(pixel_pos);
    let metallic = metallic_roughness_occlusion.r;
    let roughness = metallic_roughness_occlusion.g;
    let baked_occlusion = metallic_roughness_occlusion.b;
    let screen_space_occlusion = params.gtao_image.Load(pixel_pos).r;
    let occlusion = baked_occlusion * screen_space_occlusion;

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(params.camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let has_atmosphere = (scene_flags & SceneFlags::HasAtmosphere) != 0;
    let has_directional_light = (scene_flags & SceneFlags::HasDirectionalLight) != 0;

    // PBR constants
    let Li = has_directional_light ? params.lights.directional_light.intensity : 1.0;
    let L = has_directional_light ? params.lights.directional_light.direction : f32x3(0.0, 1.0, 0.0);
    let V = normalize(params.camera.position.xyz - world_position);
    let N = normalize(mapped_normal);
    let R = reflect(-V, N);

    var indirect_illuminance = f32x3(0.0);
    var sun_illuminance = f32x3(1.0);
    if (has_atmosphere) {
        let atmosphere = params.lights.atmosphere;
        let planet_radius = atmosphere.planet_radius;
        let atmos_radius = atmosphere.atmos_radius;

        var eye_altitude = max(world_position.y, 0.0) * CAMERA_SCALE_UNIT;
        eye_altitude += planet_radius + PLANET_RADIUS_OFFSET;

        let sun_cos_theta = dot(L, f32x3(0.0, 1.0, 0.0));
        let horizon_darkening = smoothstep(-0.1, 0.3, sun_cos_theta);

        let transmittance_uv = transmittance_params_to_lut_uv(
            atmos_radius,
            planet_radius,
            f32x2(eye_altitude, sun_cos_theta));
        let sun_transmittance = params.sky_transmittance_lut.SampleLevel(params.linear_clamp_sampler, transmittance_uv, 0.0).rgb;
        sun_illuminance = sun_transmittance * Li * horizon_darkening;
    }

    let base_ambient_color = has_directional_light ? params.lights.directional_light.color : f32x3(0.02);
    let ambient_contribution = base_ambient_color * albedo_color * sun_illuminance;

    var total_lighting = f32x3(0.0, 0.0, 0.0);
    /*
    for(int point_light_idx = 0; point_light_idx < params.lights.point_light_count; point_light_idx++) {
        let light = params.lights.point_lights[point_light_idx];
        let point_light_contribution = point_light_contribution(
            world_position,
            N,
            V,
            albedo_color,
            roughness,
            metallic,
            base_ambient_color,
            light
        );

        total_lighting += point_light_contribution;
    }

    for(int spot_light_idx = 0; spot_light_idx < params.lights.spot_light_count; spot_light_idx++) {
        let light = params.lights.spot_lights[spot_light_idx];
        let spot_light_contribution = calculate_spot_light_contribution(
            world_position,
            N,
            V,
            albedo_color,
            roughness,
            metallic,
            base_ambient_color,
            light
        );

        total_lighting += spot_light_contribution;
    }
    */

    // https://marmosetco.tumblr.com/post/81245981087
    let horizon_fade = 1.3;
    var horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    var material_surface_color = f32x3(0.0);
    let NoL = max(dot(N, L), 0.0);
    if (NoL > 0.0) {
        let view_z = -mul(params.camera.view, f32x4(world_position, 1.0)).z;
        var directional_shadow = 1.0f;
        if (has_directional_light && params.lights.directional_light.cascade_count > 0) {
            directional_shadow = sample_shadow_map(
                params.lights.directional_light,
                params.lights.directional_light_cascades,
                params.directional_shadows_map,
                params.linear_clamp_sampler,
                view_z,
                world_position,
                N,
                L);
        }

        var contact_shadow = 1.0f;
        let contact_shadows_enabled = (scene_flags & SceneFlags::HasContactShadows) != 0;
        if (contact_shadows_enabled) {
            contact_shadow = 1.0f - params.contact_shadows.Load(pixel_pos).r;
        }

        let brdf = BRDF(V, N, L, albedo_color, roughness, metallic, base_ambient_color);
        material_surface_color = brdf * horizon * sun_illuminance * NoL * directional_shadow * contact_shadow;
    }

    let indirect_illuminance_sum = (indirect_illuminance + ambient_contribution) * occlusion;
    let final_color = material_surface_color + total_lighting + indirect_illuminance_sum + emission;
    return f32x4(final_color, 1.0);
}
