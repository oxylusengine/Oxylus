import gpu;
import common;
import pbr;
import sky;
import scene;

#include <fullscreen.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::binding(0)]] Sampler linear_clamp_sampler;
[[vk::binding(1)]] Sampler linear_repeat_sampler;
[[vk::binding(2)]] Texture2D<f32> depth_image;
[[vk::binding(3)]] Texture2D<f32x4> albedo_image;
[[vk::binding(4)]] Texture2D<f32x4> normal_image;
[[vk::binding(5)]] Texture2D<f32x3> emissive_image;
[[vk::binding(6)]] Texture2D<f32x3> metallic_roughness_occlusion_image;
[[vk::binding(7)]] Texture2D<f32> gtao_image;
[[vk::binding(8)]] Texture2D<f32> resolved_shadows;
[[vk::binding(9)]] Texture2D<f32> contact_shadows;
[[vk::binding(10)]] ConstantBuffer<Camera> camera;


[[shader("fragment")]]
func fs_main(
  VertexOutput input,
  uniform f32x3 L,
  uniform f32 Li,
  uniform f32x3 base_ambient_color,
  uniform u32 light_count,
  uniform Light *lights,
  uniform Sky *sky,
  uniform f32 ao_direct_diffuse,
  uniform f32 ao_direct_specular,
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

    // PBR constants
    let V = normalize(camera.position.xyz - world_position);
    let N = normalize(mapped_normal);
    let R = reflect(-V, N);
    let NoV = abs(dot(N, V)) + 1e-5;
    let NoL = max(dot(N, L), 0.0);

    if ((scene_flags & SceneFlags::HasSky) != 0 && depth == 0.0) {
      return sky.has_texture ? f32x4(1.f) : f32x4(sky.solid_color);
    }

    let directional_shadow = has_directional_light ? resolved_shadows.Load(pixel_pos) : 1.0f;
    let contact_shadow = ((scene_flags & SceneFlags::HasContactShadows) != 0) ? contact_shadows.Load(pixel_pos) : 1.0f;
    let visibility = directional_shadow * contact_shadow;

    // No atmosphere, so no transmittance to attenuate the sun.
    var indirect_illuminance = f32x3(0.0);
    var direct_illuminance = has_directional_light ? f32x3(Li) : f32x3(0.0);

    let has_sky = (scene_flags & SceneFlags::HasSky) != 0;
    let environment_radiance = has_sky ? sky.ambient_color : base_ambient_color;
    var diffuse_sky = environment_radiance;
    var specular_sky = environment_radiance;

    {
        let dielectric_F0 = f32x3(0.04);
        let F0 = lerp(dielectric_F0, albedo_color, metallic);

        let brdf = env_brdf(NoV, roughness, F0);
        let diffuse_albedo = albedo_color * (1.0 - metallic);
        let kD = diffuse_albedo * saturate(1.0 - brdf.single_scatter - brdf.multi_scatter);

        let spec_occlusion = specular_occlusion_horizon(NoV, occlusion, roughness);

        let ibl_diffuse = kD * diffuse_sky * occlusion;
        let ibl_specular = (brdf.single_scatter * specular_sky * spec_occlusion)
            + (brdf.multi_scatter * diffuse_sky * spec_occlusion);
        indirect_illuminance = ibl_diffuse + ibl_specular;
    }

    var total_lighting = f32x3(0.0, 0.0, 0.0);
    for (u32 light_index = 0; light_index < light_count; light_index++) {
        let light = lights[light_index];

        if (light.kind == LightKind::Point) {
            total_lighting += point_light_contribution(
                world_position, N, V, albedo_color, roughness, metallic, light.position, light);
        } else if (light.kind == LightKind::Spot) {
            total_lighting += calculate_spot_light_contribution(
                world_position, N, V, albedo_color, roughness, metallic, light.position, light.direction, light);
        }
    }

    let horizon_fade = 1.3;
    var horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    let direct_diffuse_ao = lerp(1.0, occlusion, saturate(ao_direct_diffuse));
    let direct_specular_ao = lerp(
        1.0, specular_occlusion_horizon(NoV, occlusion, roughness), saturate(ao_direct_specular));

    var material_surface_color = f32x3(0.0);
    if (NoL > 0.0) {
        let result = BRDF(V, N, L, albedo_color, roughness, metallic);
        let diffuse_term = result.diffuse * direct_diffuse_ao;
        let specular_term = result.specular * horizon * direct_specular_ao;
        material_surface_color = (diffuse_term + specular_term) * direct_illuminance * NoL * visibility;
    }

    let punctual_lighting = total_lighting * direct_diffuse_ao;

    let final_color = material_surface_color + punctual_lighting + indirect_illuminance + emission;
    return f32x4(final_color, 1.0);
}
