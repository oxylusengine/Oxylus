import gpu;
import common;
import pbr;
import scene;
import rmvsm;
import fog;

#include <rmvsm_pointspot.slang>
#include <rmvsm_directional.slang>

public struct LightingResources {
    public Texture2DArray<u32> vsm_page_table;
    public Texture2DArray<u32> vsm_pointspot_page_tables;
    public Texture2D<f32> vsm_physical_pages;
    public SamplerState nearest_clamp_sampler;
    public Camera camera;
    public PBRLighting *lighting;
    public Light *lights;
    public LightGridCell *light_grid;
    public VSMPointSpotView *vsm_pointspot_views;
    public VirtualClipmap *vsm_clipmaps;

    public __init(
        Texture2DArray<u32> vsm_page_table,
        Texture2DArray<u32> vsm_pointspot_page_tables,
        Texture2D<f32> vsm_physical_pages,
        SamplerState nearest_clamp_sampler,
        Camera camera,
        PBRLighting *lighting,
        Light *lights,
        LightGridCell *light_grid,
        VSMPointSpotView *vsm_pointspot_views,
        VirtualClipmap *vsm_clipmaps
    ) {
        this.vsm_page_table = vsm_page_table;
        this.vsm_pointspot_page_tables = vsm_pointspot_page_tables;
        this.vsm_physical_pages = vsm_physical_pages;
        this.nearest_clamp_sampler = nearest_clamp_sampler;
        this.camera = camera;
        this.lighting = lighting;
        this.lights = lights;
        this.light_grid = light_grid;
        this.vsm_pointspot_views = vsm_pointspot_views;
        this.vsm_clipmaps = vsm_clipmaps;
    }
};

public func lighting_directional_visibility(
    in LightingResources res,
    u32 scene_flags,
    u32 has_directional_shadows,
    f32x2 uv,
    f32x2 frag_coord,
    f32 depth,
    f32x3 smooth_normal,
    f32x3 world_position,
    bool has_directional_light
) -> f32 {
    var directional = 1.0;
    if (has_directional_light && has_directional_shadows != 0 && depth != 0.0) {
        let directional_resources = VSMDirectionalResources(
            res.vsm_clipmaps, res.vsm_page_table, res.vsm_physical_pages, res.nearest_clamp_sampler, scene_flags);
        directional = vsm_directional_visibility(
            *res.lighting, directional_resources, res.camera, uv, frag_coord, depth, normalize(smooth_normal), world_position);
    }

    return directional;
}

public func lighting_point_spot(
    in LightingResources res,
    f32x2 uv,
    f32 depth,
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 smooth_normal,
    f32x3 albedo_color,
    f32 metallic,
    in BRDFSetup brdf,
    inout f32x3 color
) -> void {
    var diffuse = f32x3(0.0);
    var specular = f32x3(0.0);
    lighting_point_spot(
        res, uv, depth, world_position, N, V, smooth_normal, albedo_color, metallic, 0.04, brdf, diffuse, specular);
    color += diffuse + specular;
}

public func lighting_point_spot(
    in LightingResources res,
    f32x2 uv,
    f32 depth,
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 smooth_normal,
    f32x3 albedo_color,
    f32 metallic,
    f32 dielectric_f0_scalar,
    in BRDFSetup brdf,
    inout f32x3 diffuse,
    inout f32x3 specular
) -> void {
    let camera = res.camera;
    let lighting = *res.lighting;
    let lights = res.lights;
    let light_grid = res.light_grid;
    let vsm_pointspot_views = res.vsm_pointspot_views;
    let vsm_pointspot_page_tables = res.vsm_pointspot_page_tables;
    let vsm_physical_pages = res.vsm_physical_pages;

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

            let result = brdf_evaluate(brdf, V, N, L, albedo_color, metallic, dielectric_f0_scalar);
            diffuse += shadow * result.diffuse * light.color * weight;
            specular += shadow * result.specular * light.color * weight;
        }
    }
}

// the sun's visibility at a point in the air. One filtered tap of the clipmap volumetric fog marked for the point, falling
// back to its neighbours, and lit where no page was rendered
public func lighting_directional_volume_visibility(
    in LightingResources res, u32 scene_flags, u32 has_directional_shadows, f32x3 world_position
) -> f32 {
    if (has_directional_shadows == 0) {
        return 1.0;
    }

    let vsm_ctx = (*res.lighting).vsm;
    let directional = VSMDirectionalResources(
        res.vsm_clipmaps, res.vsm_page_table, res.vsm_physical_pages, res.nearest_clamp_sampler, scene_flags);

    let clipmap_index = vsm_ctx.get_volume_clipmap_index(length(world_position - res.camera.position.xyz));

    let ndc0 = mul(res.vsm_clipmaps[0].projection_view_mat, f32x4(world_position, 1.0)).xyz;
    let shift0 = 2.0 * f32x2(res.vsm_clipmaps[0].page_offset) / f32(VSM_DIRECTIONAL_PAGE_TABLE_SIZE);

    VSMReceiver receiver;
    receiver.light_xy = ndc0.xy + shift0;
    receiver.depth = ndc0.z;
    receiver.slope = get_vsm_slope(vsm_ctx, vsm_ctx.directional_light_dir, 1.0);
    receiver.base_uv = get_vsm_level_uv(vsm_ctx, directional, clipmap_index, receiver.light_xy, f32x2(0.0));
    receiver.base_uv_scale = 0.5 * exp2(-f32(clipmap_index)) / vsm_ctx.first_clipmap_width;
    receiver.center = make_vsm_center_page(directional, clipmap_index, receiver.base_uv);

    return pcss_hard_shadow(vsm_ctx, directional, receiver, clipmap_index);
}

// radiance point and spot lights scatter toward the eye from a point in the air, per unit of scattering coefficient.
// Shadows come from the coarsest mip, which every active layer keeps, the same lookup DDGI's probes make
public func lighting_point_spot_volume(
    in LightingResources res, f32x3 world_position, f32x3 view_direction, f32 anisotropy
) -> f32x3 {
    let lighting = *res.lighting;
    let grid_cell = light_grid_cell_coords(world_position, lighting.light_grid_origin);
    let is_grid_cell_in_bounds = light_grid_cell_in_bounds(grid_cell);
    let grid_cell_index = light_grid_cell_index(grid_cell);

    var radiance = f32x3(0.0);
    let light_count = min(lighting.light_count, MAX_LIGHTS);
    let word_count = (light_count + 31u) / 32u;
    [loop]
    for (u32 word = 0; word < word_count; word++) {
        var lane_word = 0xFFFFFFFFu;
        if (is_grid_cell_in_bounds) {
            lane_word = res.light_grid[grid_cell_index].light_word(word);
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

            let light = res.lights[word * 32u + bit];
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

            var shadow = 1.0;
            if (light.shadow_map_index >= 0) {
                let layer = light.kind == LightKind::Point
                    ? vsm_point_layer(light.shadow_map_index, vsm_point_face_index(-light_vector))
                    : vsm_spot_layer(light.shadow_map_index);
                let view = res.vsm_pointspot_views[layer];
                if (view.range > 0.0 && distance_sq <= view.range * view.range) {
                    let center = view.project(world_position);
                    if (center.valid) {
                        // nudged toward the light so the air right in front of a caster doesn't shadow itself
                        let biased = view.project(world_position + L * min(0.1, 0.5 * light_distance));
                        let reference_depth = biased.valid ? biased.depth : center.depth;
                        let shadow_depth = vsm_pointspot_load_depth(
                            res.vsm_pointspot_page_tables,
                            res.vsm_physical_pages,
                            layer,
                            VSM_POINT_SPOT_MIP_COUNT - 1,
                            center.uv
                        );
                        shadow = shadow_depth < reference_depth ? 0.0 : 1.0;
                    }
                }
            }

            radiance += light.color * (attenuation * shadow * henyey_greenstein(dot(L, view_direction), anisotropy));
        }
    }

    return radiance;
}
