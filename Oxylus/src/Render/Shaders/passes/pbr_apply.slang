import gpu;
import common;
import pbr;
import sky;
import scene;
import shadows;

#include <fullscreen.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::binding(0)]] Sampler linear_clamp_sampler;
[[vk::binding(1)]] Sampler linear_repeat_sampler;
[[vk::binding(2)]] Texture2D<f32x4> sky_transmittance_lut;
[[vk::binding(3)]] Texture3D<f32x4> sky_aerial_perspective_lut;
[[vk::binding(4)]] Texture2D<f32x4> sky_view_lut;
[[vk::binding(5)]] Texture2D<f32> depth_image;
[[vk::binding(6)]] Texture2D<f32x4> albedo_image;
[[vk::binding(7)]] Texture2D<f32x4> normal_image;
[[vk::binding(8)]] Texture2D<f32x3> emissive_image;
[[vk::binding(9)]] Texture2D<f32x3> metallic_roughness_occlusion_image;
[[vk::binding(10)]] Texture2D<f32> gtao_image;
[[vk::binding(11)]] Texture2D<f32> contact_shadows;
[[vk::binding(12)]] Texture2DArray<f32> directional_shadows_map;
[[vk::binding(13)]] ConstantBuffer<Lights> lights;
[[vk::binding(14)]] ConstantBuffer<Camera> camera;
[[vk::binding(15)]] TextureCube<f32x4> sky_cubemap;

func draw_sun(f32x3 world_dir, f32x3 sun_dir, f32 radius) -> f32x3 {
    let min_cos_theta = cos(radius * PI / 180.0);

    let cosTheta = dot(world_dir, sun_dir);
    if (cosTheta >= min_cos_theta) {
        return 1.0;
    }

    let offset = min_cos_theta - cosTheta;
    let gaussianBloom = exp(-offset * 50000.0) * 0.5;
    let invBloom = 1.0/(0.02 + offset * 300.0) * 0.01;
    return f32x3(gaussianBloom + invBloom);
}

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

    let result = BRDF(V, N, L, albedo_color, roughness, metallic);
    f32x3 radiance = light.color * attenuation * light.intensity;

    return (result.diffuse + result.specular) * radiance * NdotL;
}

f32 calculate_spot_light_attenuation(f32x3 light_direction, f32x3 spot_direction, f32 inner_cone_angle, f32 outer_cone_angle) {
    f32 cos_angle = dot(-light_direction, normalize(spot_direction));

    f32 cos_inner = cos(inner_cone_angle);
    f32 cos_outer = cos(outer_cone_angle);

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

    let result = BRDF(V, N, L, albedo_color, roughness, metallic);
    f32x3 radiance = light.color * total_attenuation * light.intensity;

    return (result.diffuse + result.specular) * radiance * NdotL;
}

[[shader("fragment")]]
func fs_main(
    VertexOutput input
) -> f32x4 {
    let pixel_pos = u32x3(u32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);

    let albedo_color = albedo_image.SampleLevel(linear_repeat_sampler, input.tex_coord, 0).rgb;

    let mapped_smooth_normal = normal_image.Load(pixel_pos);
    let mapped_normal = com::oct_to_vec3(mapped_smooth_normal.rg);
    let smooth_normal = com::oct_to_vec3(mapped_smooth_normal.ba);

    let emission = emissive_image.Load(pixel_pos);

    let metallic_roughness_occlusion = metallic_roughness_occlusion_image.Load(pixel_pos);
    let metallic = metallic_roughness_occlusion.r;
    let roughness = metallic_roughness_occlusion.g;
    let baked_occlusion = metallic_roughness_occlusion.b;
    let screen_space_occlusion = gtao_image.Load(pixel_pos).r;
    let occlusion = baked_occlusion * screen_space_occlusion;

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let has_atmosphere = (scene_flags & SceneFlags::HasAtmosphere) != 0;
    let has_directional_light = (scene_flags & SceneFlags::HasDirectionalLight) != 0;

    // PBR constants
    let Li = has_directional_light ? lights.directional_light.intensity : 1.0;
    let L = has_directional_light ? lights.directional_light.direction : f32x3(0.0, 1.0, 0.0);
    let V = normalize(camera.position.xyz - world_position);
    let N = normalize(mapped_normal);
    let R = reflect(-V, N);
    let NoV = abs(dot(N, V)) + 1e-5;
    let NoL = max(dot(N, L), 0.0);

    var directional_shadow = 1.0f;
    if (has_directional_light && lights.directional_light.cascade_count > 0) {
        let view_z = -mul(camera.view, f32x4(world_position, 1.0)).z;
        directional_shadow = sample_shadow_map(
            lights.directional_light,
            lights.directional_light_cascades,
            directional_shadows_map,
            linear_clamp_sampler,
            view_z,
            world_position,
            N,
            L);
    }

    var indirect_illuminance = f32x3(0.0);
    var sun_illuminance = f32x3(Li);
    if (has_atmosphere) {
        let atmosphere = lights.atmosphere;
        let planet_radius = atmosphere.planet_radius;
        let atmos_radius = atmosphere.atmos_radius;

        var eye_altitude = max(world_position.y, 0.0) * CAMERA_SCALE_UNIT;
        eye_altitude += planet_radius + PLANET_RADIUS_OFFSET;

        let up = f32x3(0.0, 1.0, 0.0);
        let sun_cos_theta = dot(L, up);
        let horizon_darkening = smoothstep(-0.1, 0.3, sun_cos_theta);

        let transmittance_uv = transmittance_params_to_lut_uv(
            atmos_radius,
            planet_radius,
            f32x2(eye_altitude, sun_cos_theta));
        let sun_transmittance = sky_transmittance_lut.SampleLevel(linear_clamp_sampler, transmittance_uv, 0.0).rgb;
        sun_illuminance = sun_transmittance * Li * horizon_darkening;

        if (depth == 0.0) {
            let eye_pos = lights.atmosphere.eye_pos;
            let h = eye_pos.y;
            let right = normalize(cross(up, -V));
            let forward = normalize(cross(right, up));
            let light_on_plane = normalize(f32x2(dot(L, forward), dot(L, right)));
            let view_zenith_cos_angle = dot(-V, up);
            let planet_intersection = com::ray_sphere_intersect_nearest(eye_pos, -V, planet_radius);
            let sky_view_uv = sky_view_params_to_lut_uv(
                *lights.atmosphere, planet_intersection.hasValue, h, view_zenith_cos_angle, light_on_plane);
            var sky_view = sky_view_lut.SampleLevel(linear_clamp_sampler, sky_view_uv, 0.0);
            if (!planet_intersection.hasValue) {
                sky_view.rgb += draw_sun(-V, L, 1.0) * Li * sun_transmittance;
            }

            return f32x4(sky_view.rgb * sky_view.a, 1.0);
        }

        let aerial_perspective = sample_aerial_perspective(
            *lights.atmosphere,
            sky_aerial_perspective_lut,
            linear_clamp_sampler,
            input.tex_coord,
            (world_position - camera.position.xyz) * CAMERA_SCALE_UNIT);

        let dielectric_F0 = f32x3(0.04);
        let F0 = lerp(dielectric_F0, albedo_color, metallic);
        let F = F_SchlickRoughness(NoV, F0, roughness);
        let kD = (1.0 - metallic) * (1.0 - F);

        let diffuse_irradiance = sky_cubemap.SampleLevel(linear_clamp_sampler, N, 0.0).rgb;
        let ibl_diffuse = kD * albedo_color * diffuse_irradiance;

        let reflection_dir = normalize(lerp(N, R, 1.0 - roughness));
        let specular_radiance = sky_cubemap.SampleLevel(linear_clamp_sampler, reflection_dir, 0.0).rgb;
        let specular_strength = (1.0 - roughness) * (1.0 - roughness);

        let spec_ao = specular_occlusion_horizon(NoV, occlusion, roughness);

        let SPECULAR_SHADOW_STRENGTH = 1.0;
        let specular_shadow_vis = has_directional_light
            ? lerp(1.0, directional_shadow, SPECULAR_SHADOW_STRENGTH)
            : 1.0;

        let ibl_specular = F * specular_radiance * specular_strength * spec_ao * specular_shadow_vis;

        indirect_illuminance = aerial_perspective.rgb * aerial_perspective.a + ibl_diffuse + ibl_specular;
    }

    let base_ambient_color = has_directional_light ? lights.directional_light.color : f32x3(0.02);
    let ambient_contribution = has_atmosphere ? f32x3(0.0) : base_ambient_color * albedo_color * sun_illuminance;

    var total_lighting = f32x3(0.0, 0.0, 0.0);
    for(int point_light_idx = 0; point_light_idx < lights.point_light_count; point_light_idx++) {
        let light = lights.point_lights[point_light_idx];
        total_lighting += point_light_contribution(
            world_position,
            N,
            V,
            albedo_color,
            roughness,
            metallic,
            light
        );
    }

    for(int spot_light_idx = 0; spot_light_idx < lights.spot_light_count; spot_light_idx++) {
        let light = lights.spot_lights[spot_light_idx];
        total_lighting += calculate_spot_light_contribution(
            world_position,
            N,
            V,
            albedo_color,
            roughness,
            metallic,
            light
        );
    }

    // https://marmosetco.tumblr.com/post/81245981087
    let horizon_fade = 1.3;
    var horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    var contact_shadow = 1.0f;
    if ((scene_flags & SceneFlags::HasContactShadows) != 0) {
        contact_shadow = 1.0f - contact_shadows.Load(pixel_pos).r;
    }

    var material_surface_color = f32x3(0.0);
    if (NoL > 0.0) {
        let result = BRDF(V, N, L, albedo_color, roughness, metallic);
        material_surface_color = (result.diffuse + result.specular) * horizon * sun_illuminance * NoL * directional_shadow * contact_shadow;
    }

    let indirect_illuminance_sum = (indirect_illuminance + ambient_contribution) * occlusion;
    let final_color = material_surface_color + total_lighting + indirect_illuminance_sum + emission;
    return f32x4(final_color, 1.0);
}
