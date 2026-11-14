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
    const f32x3 ws = camera.unproject_uv(uv, depth_sample);

    f32 rcp_near_plane_distance = 1.0f / camera.near_clip;

    f32x3 shadow_direction = normalize(lighting.sun_direction);
    f32x3 shadow_end = ws + shadow_direction * lighting.contact_shadow_length;
    // march in the jittered grid that the depth texture was rasterized into
    var jittered_projection = camera.projection;
    jittered_projection[0] += (2.0 * camera.temporalaa_jitter.x / camera.resolution.x) * camera.projection[3];
    jittered_projection[1] += (2.0 * camera.temporalaa_jitter.y / camera.resolution.y) * camera.projection[3];

    DepthRayMarchResult raymarch_result = DepthRayMarch
      ::new_from_depth(image_size)
      .from_cs(cs)
      .to_ws(shadow_end, camera.view, jittered_projection)
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
        let resources = VSMDirectionalResources(vsm_clipmaps, vsm_page_table, vsm_physical_pages, nearest_clamp_sampler);
        directional = vsm_directional_visibility(
            lighting, resources, camera, uv, frag_coord, depth, normalize(smooth_normal), world_position);
    }

    var contact = 1.0;
    if ((scene_flags & SceneFlags::HasContactShadows) != 0) {
        contact = contact_shadow_visibility(uv, lighting);
    }

    return directional * contact;
}

func point_spot_lighting(
    f32x2 uv,
    f32 depth,
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 smooth_normal,
    f32x3 albedo_color,
    f32 metallic,
    in BRDFSetup brdf,
    in PBRLighting lighting,
    Light *lights,
    LightGridCell *light_grid,
    VSMPointSpotView *vsm_pointspot_views,
    inout f32x3 color
) -> void {
    let grid_cell = light_grid_cell_coords(world_position, lighting.light_grid_origin);
    let is_grid_cell_in_bounds = light_grid_cell_in_bounds(grid_cell);
    let grid_cell_index = light_grid_cell_index(grid_cell);

    let light_count = min(lighting.light_count, MAX_LIGHTS);
    let word_count = (light_count + 31u) / 32u;
    [loop]
    for (u32 word = 0; word < word_count; word++) {
        var lane_word = 0xFFFFFFFFu;
        if (is_grid_cell_in_bounds) {
            lane_word = light_grid[grid_cell_index].light_word(word);
        } else {
            let remaining = light_count - word * 32u;
            lane_word = remaining >= 32u ? 0xFFFFFFFFu : (1u << remaining) - 1u;
        }

        var wave_bits = WaveActiveBitOr(lane_word);
        while (wave_bits != 0u) {
            let bit = firstbitlow(wave_bits);
            wave_bits &= wave_bits - 1u;
            if (((lane_word >> bit) & 1u) == 0u) {
                continue;
            }

            let light = lights[word * 32u + bit];

            let light_vector = light.position - world_position;
            let distance_sq = dot(light_vector, light_vector);
            if (light.intensity <= 0.0 || (light.range > 0.0 && distance_sq >= light.range * light.range)) {
                continue;
            }

            let light_distance = sqrt(distance_sq);
            let L = light_vector / light_distance;

            var attenuation = light.intensity * lights_attenuate_point(light_distance, light.range);
            if (light.kind == LightKind::Spot) {
                attenuation *= calculate_spot_light_attenuation(L, light.direction, light.inner_cone_angle, light.outer_cone_angle);
            }

            if (attenuation <= 0.0) {
                continue;
            }

            let NoL = saturate(dot(N, L));
            if (NoL <= 0.0) {
                continue;
            }

            let weight = attenuation * NoL;

            var shadow = 1.0;
            if (light.shadow_map_index >= 0) {
                let to_pixel = -light_vector;
                let my_face = light.kind == LightKind::Point ? vsm_point_face_index(to_pixel) : 0;
                var is_pending = true;
                while (WaveActiveAnyTrue(is_pending)) {
                    if (!is_pending) {
                        continue;
                    }

                    let face = WaveReadLaneFirst(my_face);
                    if (my_face != face) {
                        continue;
                    }

                    is_pending = false;
                    let layer = light.kind == LightKind::Point
                        ? vsm_point_layer(light.shadow_map_index, face)
                        : vsm_spot_layer(light.shadow_map_index);

                    let view = vsm_pointspot_views[layer];
                    if (view.range > 0.0 && distance_sq <= view.range * view.range) {
                        let center = view.project(world_position);
                        if (center.valid) {
                            // must match the VSM marking footprint
                            let footprint_uv_offset = (1.0 / camera.resolution) * 0.5;
                            let footprint_left = camera.unproject_uv(
                                uv + f32x2(-footprint_uv_offset.x, footprint_uv_offset.y), depth);
                            let footprint_right = camera.unproject_uv(
                                uv + f32x2(footprint_uv_offset.x, footprint_uv_offset.y), depth);
                            let uv_left = view.project(footprint_left).uv;
                            let uv_right = view.project(footprint_right).uv;
                            let mip = vsm_pointspot_mip(uv_left, uv_right, lighting.vsm_mip_bias_min);

                            let pcf_rotation = 6.28318530718 *
                                               fract(52.9829189 * fract(dot(floor(uv * camera.resolution), f32x2(0.06711056, 0.00583715))));

                            shadow = vsm_sample_pointspot_shadow(
                                view,
                                layer,
                                mip,
                                vsm_pointspot_page_tables,
                                vsm_physical_pages,
                                world_position,
                                smooth_normal,
                                L,
                                pcf_rotation
                            );
                        }
                    }
                }
            }

            let result = brdf_evaluate(brdf, V, N, L, albedo_color, metallic);
            color += shadow * (result.diffuse + result.specular) * light.color * weight;
        }
    }
}
