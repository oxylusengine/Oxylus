import gpu;
import common;
import pbr;
import sky;
import scene;

#include <fullscreen.slang>

#include <pbr_apply_common.slang>

[[shader("fragment")]]
func fs_main(
  VertexOutput input,
  // keep the pointer 8-byte aligned under scalar layout
  uniform Sky *sky,
  uniform PBRLighting *lighting,
  uniform Light *lights,
  uniform u32x4 *light_grid,
  uniform VSMPointSpotView *vsm_pointspot_views,
  uniform VirtualClipmap *vsm_clipmaps,
) -> f32x4 {
    let pixel_pos = u32x3(u32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);

    if ((scene_flags & SceneFlags::TransparentBackground) != 0 && depth == 0.0) {
        return f32x4(0.0);
    }

    if ((scene_flags & SceneFlags::HasSky) != 0 && depth == 0.0) {
      return sky.has_texture != 0 ? f32x4(1.0) : f32x4(sky.solid_color);
    }

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let L = lighting.sun_direction;
    let Li = lighting.sun_intensity;
    let has_directional_light = (scene_flags & SceneFlags::HasDirectionalLight) != 0;

    let mapped_smooth_normal = normal_image.Load(pixel_pos);
    let mapped_normal = com::oct_to_vec3(mapped_smooth_normal.rg);
    let smooth_normal = com::oct_to_vec3(mapped_smooth_normal.ba);
    let N = normalize(mapped_normal);
    let NoL = max(dot(N, L), 0.0);

    // shadows run before the material reads so those don't stay live across the shadow loops
    var visibility = 0.0;
    if (NoL > 0.0) {
        visibility = sun_visibility(
            input.tex_coord, input.position.xy, depth, smooth_normal, world_position, has_directional_light, *lighting, vsm_clipmaps);
    }

    let albedo_occlusion = albedo_image.SampleLevel(linear_repeat_sampler, input.tex_coord, 0);
    let albedo_color = albedo_occlusion.rgb;

    let emission = emissive_image.Load(pixel_pos);

    let metallic_roughness = metallic_roughness_image.Load(pixel_pos);
    let metallic = clamp(metallic_roughness.r, 0.0, 1.0);
    let roughness = clamp(metallic_roughness.g, 0.045, 1.0);

    let baked_occlusion = albedo_occlusion.a;
    let screen_space_occlusion = gtao_image.Load(pixel_pos).r;
    let occlusion = baked_occlusion * screen_space_occlusion;

    let V = normalize(camera.position.xyz - world_position);
    let R = reflect(-V, N);
    let NoV = abs(dot(N, V)) + 1e-5;

    // no atmosphere, so no transmittance to attenuate the sun
    let direct_illuminance = has_directional_light ? f32x3(Li) : f32x3(0.0);

    var indirect_illuminance = f32x3(0.0);
    if ((scene_flags & SceneFlags::HasDDGI) == 0) {
        let has_sky = (scene_flags & SceneFlags::HasSky) != 0;
        let environment_radiance = has_sky ? sky.ambient_color : lighting.base_ambient_color;

        let dielectric_F0 = f32x3(0.04);
        let F0 = lerp(dielectric_F0, albedo_color, metallic);

        let alpha = perceptual_roughness_to_alpha(roughness);
        let AB = GGX_directional_albedo(NoV, alpha);
        let kS = F0 * AB.x + AB.y;
        let kD = (1.0 - metallic) * (1.0 - kS);

        let spec_occlusion = specular_occlusion_horizon(NoV, occlusion, roughness);

        let ibl_diffuse = kD * environment_radiance * albedo_color;
        let ibl_specular = kS * environment_radiance * spec_occlusion;
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

    var color = material_surface_color + indirect_illuminance + emission;
    if (depth != 0.0) {
        point_spot_lighting(
            input.tex_coord, u32x2(input.position.xy), depth, world_position, N, V, smooth_normal, albedo_color, roughness, metallic,
            *lighting, lights, light_grid, vsm_pointspot_views, color);
    }

    return f32x4(color, 1.0);
}
