// includer provides imports common, gpu, scene, hit, pbr, sky, ddgi, rmvsm
// globals scene_tlas, lights, sky_view_lut, sky_transmittance_lut, linear_clamp_sampler, pointspot_page_table, vsm_physical_pages, probe_volumes, probe_states, irradiance_atlas, distance_atlas
// spec constant scene_flags, constants with pointspot_views, light_grid, light_grid_origin, and trace_occluded

func sun_illuminance(Atmosphere *atmosphere, f32x3 world_position, f32x3 sun_direction, f32 sun_intensity) -> f32x3 {
    let sun_cos_theta = dot(sun_direction, f32x3(0.0, 1.0, 0.0));
    let horizon_darkening = smoothstep(-0.1, 0.3, sun_cos_theta);
    let eye_position = atmosphere.get_eye_pos(world_position);
    let transmittance = get_direct_sun_lighting(
        atmosphere,
        linear_clamp_sampler,
        sky_transmittance_lut,
        eye_position.y,
        sun_cos_theta
    );

    return transmittance * sun_intensity * horizon_darkening;
}

func miss_radiance(
    Atmosphere *atmosphere,
    f32x3 origin,
    f32x3 direction,
    f32x3 sun_direction,
    f32x3 ambient_color
) -> f32x3 {
    if ((scene_flags & SceneFlags::HasAtmosphere) == 0) {
        return ambient_color;
    }

    return get_atmosphere_illuminance_along_ray(
        atmosphere,
        linear_clamp_sampler,
        sky_view_lut,
        sun_direction,
        atmosphere.get_eye_pos(origin),
        direction
    );
}

func punctual_shadow_visibility(
    in Light light,
    f32x3 position,
    f32x3 receiver_normal,
    f32x3 to_light_direction,
    f32 receiver_bias
) -> f32 {
    if (light.shadow_map_index < 0) {
        return 1.0;
    }

    let layer = light.kind == LightKind::Point
        ? vsm_point_layer(light.shadow_map_index, vsm_point_face_index(position - light.position))
        : vsm_spot_layer(light.shadow_map_index);
    let view = constants.pointspot_views[layer];
    if (view.range <= 0.0) {
        return 1.0;
    }

    let receiver_position = position + receiver_normal * receiver_bias;
    let center = view.project(receiver_position);
    if (!center.valid) {
        return 1.0;
    }

    let biased = view.project(receiver_position + to_light_direction * receiver_bias);
    let reference_depth = biased.valid ? biased.depth : center.depth;
    let coarse_mip = VSM_POINT_SPOT_MIP_COUNT - 1;
    let shadow_depth = vsm_pointspot_load_depth(
        pointspot_page_table,
        vsm_physical_pages,
        layer,
        coarse_mip,
        center.uv
    );

    return shadow_depth < reference_depth ? 0.0 : 1.0;
}

func punctual_diffuse(
    in Light light,
    f32x3 position,
    f32x3 normal,
    f32x3 receiver_normal,
    f32x3 diffuse_albedo,
    f32 receiver_bias
) -> f32x3 {
    if (light.intensity <= 0.0) {
        return f32x3(0.0);
    }

    let light_vector = light.position - position;
    let light_distance = length(light_vector);
    if (light_distance <= 1e-4) {
        return f32x3(0.0);
    }
    let L = light_vector / light_distance;

    let NoL = saturate(dot(normal, L));
    if (NoL <= 0.0) {
        return f32x3(0.0);
    }

    var attenuation = lights_attenuate_point(light_distance, light.range);
    if (light.kind == LightKind::Spot) {
        attenuation *= calculate_spot_light_attenuation(
            L,
            light.direction,
            light.inner_cone_angle,
            light.outer_cone_angle
        );
    }
    if (attenuation <= 0.0) {
        return f32x3(0.0);
    }

    let visibility = punctual_shadow_visibility(light, position, receiver_normal, L, receiver_bias);
    if (visibility <= 0.0) {
        return f32x3(0.0);
    }

    return diffuse_albedo * Fd_Lambert() * light.color * light.intensity * attenuation * NoL * visibility;
}

func shade_hit(
    in RayHit hit,
    u32 volume_count,
    f32x3 view_direction,
    f32 view_distance,
    f32x3 sun_direction,
    f32 sun_intensity,
    f32 shadow_ray_offset,
    f32 normal_bias,
    f32 view_bias,
    u32 light_count,
    u32 bounce_valid,
    Atmosphere *atmosphere
) -> f32x3 {
    let N = hit.surface.normal;
    let diffuse_albedo = hit.surface.albedo.rgb * (1.0 - hit.surface.metallic);
    let shadow_origin = hit.position + hit.face_normal * shadow_ray_offset;

    var radiance = hit.surface.emissive;

    let NoL = saturate(dot(N, sun_direction));
    if ((scene_flags & SceneFlags::HasDirectionalLight) != 0 && NoL > 0.0) {
        if (!trace_occluded(shadow_origin, sun_direction, shadow_ray_offset, com::HALF_MAX)) {
            let illuminance = sun_illuminance(atmosphere, hit.position, sun_direction, sun_intensity);
            radiance += diffuse_albedo * Fd_Lambert() * illuminance * NoL;
        }
    }

    let grid_cell = light_grid_cell_coords(hit.position, constants.light_grid_origin);
    let grid_cell_in_bounds = light_grid_cell_in_bounds(grid_cell);
    let grid_mask = grid_cell_in_bounds ? constants.light_grid[light_grid_cell_index(grid_cell)].shadow_mask : u32x4(0);

    for (u32 light_index = 0; light_index < light_count; light_index++) {
        let light = lights[light_index];
        if (light.kind == LightKind::Point || light.kind == LightKind::Spot) {
            if (light.shadow_map_index >= 0 && grid_cell_in_bounds) {
                let word = light_grid_mask_word(light.kind, light.shadow_map_index);
                if ((grid_mask[word] & light_grid_mask_bit(light.shadow_map_index)) == 0) {
                    continue;
                }
            }

            radiance += punctual_diffuse(
                light,
                hit.position,
                N,
                hit.face_normal,
                diffuse_albedo,
                shadow_ray_offset
            );
        }
    }

    if (bounce_valid != 0) {
        ProbeSampleParams params;
        params.world_position = hit.position;
        params.normal = N;
        params.geometric_normal = hit.face_normal;
        params.view_direction = view_direction;
        params.view_distance = view_distance;
        params.specular_direction = N;
        params.want_specular = false;
        params.normal_bias = normal_bias;
        params.view_bias = view_bias;

        let bounce = sample_ddgi_irradiance(
            probe_volumes,
            probe_states,
            volume_count,
            irradiance_atlas,
            distance_atlas,
            linear_clamp_sampler,
            params
        );
        radiance += diffuse_albedo * bounce.irradiance * bounce.coverage;
    }

    return radiance;
}
