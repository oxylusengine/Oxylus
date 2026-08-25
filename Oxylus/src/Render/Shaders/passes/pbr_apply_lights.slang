import gpu;
import common;
import pbr;
import scene;
import rmvsm;

#include <fullscreen.slang>
#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1)]] Texture2D<f32> depth_image;
[[vk::binding(2)]] Texture2D<f32x4> albedo_image;
[[vk::binding(3)]] Texture2D<f32x4> normal_image;
[[vk::binding(4)]] Texture2D<f32x2> metallic_roughness_image;
[[vk::binding(5)]] Texture2DArray<u32> vsm_pointspot_page_tables;
[[vk::binding(6)]] Texture2D<f32> vsm_physical_pages;

[[shader("fragment")]]
func fs_main(
  VertexOutput input,
  uniform Light *lights,
  uniform u32x4 *light_grid,
  uniform VSMPointSpotView *vsm_pointspot_views,
  uniform u32 light_count,
  uniform f32x3 light_grid_origin,
  uniform u32 vsm_mip_bias_min,
) -> f32x4 {
    let pixel_pos = u32x3(u32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);
    if (depth == 0.0) {
        return f32x4(0.0);
    }

    let albedo_color = albedo_image.Load(pixel_pos).rgb;

    let mapped_smooth_normal = normal_image.Load(pixel_pos);
    let mapped_normal = com::oct_to_vec3(mapped_smooth_normal.rg);
    let smooth_normal = com::oct_to_vec3(mapped_smooth_normal.ba);

    let metallic_roughness = metallic_roughness_image.Load(pixel_pos);
    let metallic = clamp(metallic_roughness.r, 0.0, 1.0);
    let roughness = clamp(metallic_roughness.g, 0.045, 1.0);

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let V = normalize(camera.position.xyz - world_position);
    let N = normalize(mapped_normal);

    let grid_cell = light_grid_cell_coords(world_position, light_grid_origin);
    let grid_cell_in_bounds = light_grid_cell_in_bounds(grid_cell);
    let grid_mask = grid_cell_in_bounds ? light_grid[light_grid_cell_index(grid_cell)] : u32x4(0);

    // must match the VSM marking footprint
    let footprint_uv_offset = (1.0 / camera.resolution) * 0.5;
    let footprint_left = camera.unproject_uv(input.tex_coord + f32x2(-footprint_uv_offset.x, footprint_uv_offset.y), depth);
    let footprint_right = camera.unproject_uv(input.tex_coord + f32x2(footprint_uv_offset.x, footprint_uv_offset.y), depth);

    let pcf_rotation = 6.28318530718 *
                       fract(52.9829189 * fract(dot(f32x2(pixel_pos.xy), f32x2(0.06711056, 0.00583715))));

    var total_lighting = f32x3(0.0, 0.0, 0.0);
    for (u32 light_index = 0; light_index < light_count; light_index++) {
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
                    let mip = vsm_pointspot_mip(uv_left, uv_right, vsm_mip_bias_min);

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

    return f32x4(total_lighting, 0.0);
}
