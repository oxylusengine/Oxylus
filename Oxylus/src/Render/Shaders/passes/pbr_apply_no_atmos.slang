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
  // keep the pointer 8-byte aligned under scalar layout
  uniform Sky *sky,
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

        let alpha = perceptual_roughness_to_alpha(roughness);
        let AB = GGX_directional_albedo(NoV, alpha);
        let kS = F0 * AB.x + AB.y;
        let kD = (1.0 - metallic) * (1.0 - kS);

        let spec_occlusion = specular_occlusion_horizon(NoV, occlusion, roughness);

        let ibl_diffuse = kD * diffuse_sky * albedo_color * Fd_Lambert();
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

    let final_color = material_surface_color + indirect_illuminance + emission;
    return f32x4(final_color, 1.0);
}
