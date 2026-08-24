import gpu;
import common;
import pbr;
import sky;
import scene;

#include <fullscreen.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::binding(0)]] Sampler linear_clamp_sampler;
[[vk::binding(1)]] Sampler linear_repeat_sampler;
[[vk::binding(2)]] Texture2D<f32x4> sky_transmittance_lut;
[[vk::binding(3)]] Texture3D<f32x4> sky_aerial_perspective_lut;
[[vk::binding(4)]] TextureCube<f32x4> sky_cubemap;
[[vk::binding(5)]] Texture2D<f32x4> sky_view_lut;
[[vk::binding(6)]] Texture2D<f32> depth_image;
[[vk::binding(7)]] Texture2D<f32x4> albedo_image;
[[vk::binding(8)]] Texture2D<f32x4> normal_image;
[[vk::binding(9)]] Texture2D<f32x3> emissive_image;
[[vk::binding(10)]] Texture2D<f32x3> metallic_roughness_occlusion_image;
[[vk::binding(11)]] Texture2D<f32> gtao_image;
[[vk::binding(12)]] Texture2D<f32x2> shadows_image;
[[vk::binding(13)]] ConstantBuffer<Camera> camera;

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

[[shader("fragment")]]
func fs_main(
  VertexOutput input,
  uniform Atmosphere *atmosphere,
  uniform f32x3 L,
  uniform f32 Li,
  uniform f32x3 base_ambient_color,
) -> f32x4 {
    let pixel_pos = u32x3(u32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);

    if ((scene_flags & SceneFlags::TransparentBackground) != 0 && depth == 0.0) {
        return f32x4(0.0);
    }

    let albedo_color = albedo_image.SampleLevel(linear_repeat_sampler, input.tex_coord, 0).rgb;

    let mapped_smooth_normal = normal_image.Load(pixel_pos);
    let mapped_normal = com::oct_to_vec3(mapped_smooth_normal.rg);
    let smooth_normal = com::oct_to_vec3(mapped_smooth_normal.ba);

    let emission = emissive_image.Load(pixel_pos);

    let metallic_roughness_occlusion = metallic_roughness_occlusion_image.Load(pixel_pos);
    let metallic = clamp(metallic_roughness_occlusion.r, 0.0, 1.0);
    let roughness = clamp(metallic_roughness_occlusion.g, 0.045, 1.0);

    let baked_occlusion = metallic_roughness_occlusion.b;
    let screen_space_occlusion = gtao_image.Load(pixel_pos).r;
    let occlusion = baked_occlusion * screen_space_occlusion;

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let has_directional_light = (scene_flags & SceneFlags::HasDirectionalLight) != 0;

    let V = normalize(camera.position.xyz - world_position);
    let N = normalize(mapped_normal);
    let R = reflect(-V, N);
    let NoV = abs(dot(N, V)) + 1e-5;
    let NoL = max(dot(N, L), 0.0);

    let shadow_visibility = shadows_image.Load(pixel_pos);
    let directional_shadow = has_directional_light ? shadow_visibility.x : 1.0f;
    let contact_shadow = ((scene_flags & SceneFlags::HasContactShadows) != 0) ? shadow_visibility.y : 1.0f;
    let visibility = directional_shadow * contact_shadow;

    var indirect_illuminance = f32x3(0.0);
    var direct_illuminance = f32x3(0.0);
    var aerial_perspective_contribution = f32x3(0.0);

    let has_sky = (scene_flags & SceneFlags::HasSky) != 0;
    var diffuse_sky = base_ambient_color;
    var specular_sky = base_ambient_color;

    let planet_radius = atmosphere.planet_radius;
    let atmos_radius = atmosphere.atmos_radius;
    let world_eye_pos = atmosphere.get_eye_pos(world_position);

    let up = f32x3(0.0, 1.0, 0.0);
    let sun_cos_theta = dot(L, up);
    let horizon_darkening = smoothstep(-0.1, 0.3, sun_cos_theta);

    let sun_transmittance = get_direct_sun_lighting(atmosphere, linear_clamp_sampler, sky_transmittance_lut, world_eye_pos.y, sun_cos_theta);
    direct_illuminance = sun_transmittance * Li * horizon_darkening;

    if (depth == 0.0) {
        let eye_pos = atmosphere.get_eye_pos(camera.position.xyz);
        let h = eye_pos.y;
        let right = normalize(cross(up, -V));
        let forward = normalize(cross(right, up));
        let light_on_plane = normalize(f32x2(dot(L, forward), dot(L, right)));
        let view_zenith_cos_angle = dot(-V, up);
        let planet_intersection = com::ray_sphere_intersect_nearest(eye_pos, -V, planet_radius);
        let sky_view_uv = sky_view_params_to_lut_uv(
            atmosphere, planet_intersection.hasValue, h, view_zenith_cos_angle, light_on_plane);
        var sky_view = sky_view_lut.SampleLevel(linear_clamp_sampler, sky_view_uv, 0.0);
        if (!planet_intersection.hasValue) {
            sky_view.rgb += draw_sun(-V, L, 1.0) * Li * sun_transmittance;
        }

        return f32x4(sky_view.rgb * sky_view.a, 1.0);
    }

    {
        let aerial_perspective = sample_aerial_perspective(
            atmosphere,
            sky_aerial_perspective_lut,
            linear_clamp_sampler,
            input.tex_coord,
            (world_position - camera.position.xyz) * CAMERA_SCALE_UNIT);
        aerial_perspective_contribution = aerial_perspective.rgb * aerial_perspective.a;

        let reflection_dir = normalize(lerp(N, R, 1.0 - roughness));
        let specular_sample = sky_cubemap.SampleLevel(linear_clamp_sampler, reflection_dir, 0.0);
        let diffuse_sample = sky_cubemap.SampleLevel(linear_clamp_sampler, N, 0.0);

        specular_sky = specular_sample.rgb * specular_sample.a * horizon_darkening;
        diffuse_sky = diffuse_sample.rgb * diffuse_sample.a * horizon_darkening;
    }

    if ((scene_flags & SceneFlags::HasDDGI) == 0) {
        let dielectric_F0 = f32x3(0.04);
        let F0 = lerp(dielectric_F0, albedo_color, metallic);

        let alpha = perceptual_roughness_to_alpha(roughness);
        let AB = GGX_directional_albedo(NoV, alpha);
        let kS = F0 * AB.x + AB.y;
        let kD = (1.0 - metallic) * (1.0 - kS);

        let spec_occlusion = specular_occlusion_horizon(NoV, occlusion, roughness);

        let ibl_diffuse = kD * diffuse_sky * albedo_color;
        let ibl_specular = kS * specular_sky * spec_occlusion;
        indirect_illuminance = (ibl_diffuse * occlusion) + ibl_specular;
    }

    let horizon_fade = 1.3;
    var horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    var material_surface_color = f32x3(0.0);
    if (NoL > 0.0) {
        let result = BRDF(V, N, L, albedo_color, roughness, metallic);
        material_surface_color = (result.diffuse + result.specular * horizon) * direct_illuminance * NoL * visibility;
    }

    let final_color = material_surface_color + indirect_illuminance + emission + aerial_perspective_contribution;
    return f32x4(final_color, 1.0);
}
