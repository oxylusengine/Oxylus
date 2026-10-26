import gpu;
import common;
import pbr;
import scene;
import rmvsm;
import raymarch;

#include <rmvsm_pointspot.slang>
#include <rmvsm_directional.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::constant_id(1)]] const u32 has_directional_shadows = 0;

[[vk::binding(0)]] Sampler linear_clamp_sampler;
[[vk::binding(1)]] Sampler linear_repeat_sampler;
[[vk::binding(2)]] Sampler nearest_clamp_sampler;
[[vk::binding(3)]] Texture2D<f32> depth_image;
[[vk::binding(4)]] Texture2D<f32x4> albedo_image;
[[vk::binding(5)]] Texture2D<f32x4> normal_image;
[[vk::binding(6)]] Texture2D<f32x3> emissive_image;
[[vk::binding(7)]] Texture2D<f32x2> metallic_roughness_image;
[[vk::binding(8)]] Texture2D<f32> gtao_image;
[[vk::binding(9)]] ConstantBuffer<Camera> camera;
[[vk::binding(10)]] Texture2DArray<u32> vsm_page_table;
[[vk::binding(11)]] Texture2DArray<u32> vsm_pointspot_page_tables;
[[vk::binding(12)]] Texture2D<f32> vsm_physical_pages;

func contact_shadow_visibility(f32x2 uv, in PBRLighting lighting) -> f32 {
    u32 width, height, levels;
    depth_image.GetDimensions(0, width, height, levels);
    let image_size = u32x2(width, height);

    f32 shadow = 0;
    f32 raymarch_jitter = 1.0;

    f32 depth_sample = depth_image.SampleLevel(nearest_clamp_sampler, uv, 0);

    const f32x3 cs = f32x3(uv * 2.0 - 1.0, depth_sample);
    const f32x4 world_position_h = mul(camera.inv_projection_view, f32x4(cs, 1.0));
    const f32x3 ws = world_position_h.xyz / world_position_h.w;

    f32 rcp_near_plane_distance = 1.0f / camera.near_clip;

    f32x3 shadow_direction = normalize(lighting.sun_direction);
    f32x3 shadow_end = ws + shadow_direction * lighting.contact_shadow_length;

    DepthRayMarchResult raymarch_result = DepthRayMarch
      ::new_from_depth(image_size)
      .from_cs(cs)
      .to_ws(shadow_end, camera.view, camera.projection)
      .with_linear_steps(lighting.contact_shadow_steps)
      .with_depth_thickness(lighting.contact_shadow_thickness)
      .with_jitter(raymarch_jitter)
      .with_march_behind_surfaces(false)
      .march(depth_image, linear_clamp_sampler, nearest_clamp_sampler, rcp_near_plane_distance);

    if (raymarch_result.hit) {
        shadow = smoothstep(1.0, 0.3, raymarch_result.hit_penetration_frac);
    }

    return 1.0 - shadow;
}

func sun_visibility(
    f32x2 uv,
    f32x2 frag_coord,
    f32 depth,
    f32x3 smooth_normal,
    f32x3 world_position,
    bool has_directional_light,
    in PBRLighting lighting,
    VirtualClipmap *vsm_clipmaps
) -> f32 {
    var directional = 1.0;
    if (has_directional_light && has_directional_shadows != 0 && depth != 0.0) {
        let resources = VSMDirectionalResources(vsm_clipmaps, vsm_page_table, vsm_physical_pages);
        directional = vsm_directional_visibility(
            lighting.vsm, resources, camera, uv, frag_coord, depth, normalize(smooth_normal), world_position);
    }

    var contact = 1.0;
    if ((scene_flags & SceneFlags::HasContactShadows) != 0) {
        contact = contact_shadow_visibility(uv, lighting);
    }

    return directional * contact;
}

func point_spot_lighting(
    f32x2 uv,
    u32x2 pixel_coords,
    f32 depth,
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 smooth_normal,
    f32x3 albedo_color,
    f32 roughness,
    f32 metallic,
    in PBRLighting lighting,
    Light *lights,
    u32x4 *light_grid,
    VSMPointSpotView *vsm_pointspot_views
) -> f32x3 {
    let grid_cell = light_grid_cell_coords(world_position, lighting.light_grid_origin);
    let grid_cell_in_bounds = light_grid_cell_in_bounds(grid_cell);
    let grid_mask = grid_cell_in_bounds ? light_grid[light_grid_cell_index(grid_cell)] : u32x4(0);

    // must match the VSM marking footprint
    let footprint_uv_offset = (1.0 / camera.resolution) * 0.5;
    let footprint_left = camera.unproject_uv(uv + f32x2(-footprint_uv_offset.x, footprint_uv_offset.y), depth);
    let footprint_right = camera.unproject_uv(uv + f32x2(footprint_uv_offset.x, footprint_uv_offset.y), depth);

    let pcf_rotation = 6.28318530718 *
                       fract(52.9829189 * fract(dot(f32x2(pixel_coords), f32x2(0.06711056, 0.00583715))));

    var total_lighting = f32x3(0.0, 0.0, 0.0);
    for (u32 light_index = 0; light_index < lighting.light_count; light_index++) {
        let light = lights[light_index];

        // a clear in-grid bit proves this light cannot affect the pixel
        if (light.shadow_map_index >= 0 && grid_cell_in_bounds) {
            let word = light_grid_mask_word(light.kind, light.shadow_map_index);
            if ((grid_mask[word] & light_grid_mask_bit(light.shadow_map_index)) == 0) {
                continue;
            }
        }

        var shadow = 1.0;
        if (light.shadow_map_index >= 0) {
            let to_pixel = world_position - light.position;
            var layer = 0;
            if (light.kind == LightKind::Point) {
                layer = vsm_point_layer(light.shadow_map_index, vsm_point_face_index(to_pixel));
            } else {
                layer = vsm_spot_layer(light.shadow_map_index);
            }

            let view = vsm_pointspot_views[layer];
            if (view.range > 0.0 && dot(to_pixel, to_pixel) <= view.range * view.range) {
                let center = view.project(world_position);
                if (center.valid) {
                    let uv_left = view.project(footprint_left).uv;
                    let uv_right = view.project(footprint_right).uv;
                    let mip = vsm_pointspot_mip(uv_left, uv_right, lighting.vsm_mip_bias_min);

                    shadow = vsm_sample_pointspot_shadow(
                        view,
                        layer,
                        mip,
                        vsm_pointspot_page_tables,
                        vsm_physical_pages,
                        world_position,
                        smooth_normal,
                        -normalize(to_pixel),
                        pcf_rotation
                    );
                }
            }
        }

        if (light.kind == LightKind::Point) {
            total_lighting += shadow * point_light_contribution(
                world_position, N, V, albedo_color, roughness, metallic, light.position, light);
        } else if (light.kind == LightKind::Spot) {
            total_lighting += shadow * calculate_spot_light_contribution(
                world_position, N, V, albedo_color, roughness, metallic, light.position, light.direction, light);
        }
    }

    return total_lighting;
}
