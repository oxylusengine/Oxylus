import common;
import gpu;
import scene;
import hit;
import pbr;
import sky;
import ddgi;
import rmvsm;

#include <rmvsm_pointspot.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene_tlas;
[[vk::binding(1, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(2, 0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3, 0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(4, 0)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(5, 0)]] StructuredBuffer<Material> materials;
[[vk::binding(6, 0)]] StructuredBuffer<Light> lights;
[[vk::binding(7, 0)]] Image2D<f32x4> sky_view_lut;
[[vk::binding(8, 0)]] Image2D<f32x4> sky_transmittance_lut;
[[vk::binding(9, 0)]] Sampler linear_clamp_sampler;
[[vk::binding(10, 0)]] RWTexture2D<f32x4, 1, ImageFormat.RGBA16F> ray_data;
[[vk::binding(11, 0)]] Image2D<f32x4> irradiance_atlas;
[[vk::binding(12, 0)]] Image2D<f32x2> distance_atlas;
[[vk::binding(13, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(14, 0)]] Texture2DArray<u32> pointspot_page_table;
[[vk::binding(15, 0)]] Texture2D<f32> vsm_physical_pages;

static const f32 HIT_TEXTURE_LOD = 3.0;

struct TraceConstants {
    Atmosphere *atmosphere;
    u32 volume_index;
    u32 volume_count;
    u32 rays_per_probe;
    u32 frame_index;
    u32 light_count;
    u32 bounce_valid;
    f32 max_ray_distance;
    f32 shadow_ray_offset;
    f32 normal_bias;
    f32 view_bias;
    f32x3 sun_direction;
    f32 sun_intensity;
    f32x3 ambient_color;
    f32 max_ray_radiance;
    u32x4 *light_grid;
    VSMPointSpotView *pointspot_views;
    u32 radiance_atlas_y_offset;
    u32 distance_culling_enabled;
    f32x3 light_grid_origin;
};

[[vk::push_constant]] ConstantBuffer<TraceConstants> constants;

struct ProbeRayPayload {
    f32x3 radiance;
    f32 hit_distance;
};

func trace_occluded(f32x3 origin, f32x3 direction, f32 t_min, f32 t_max) -> bool {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = t_min;
    ray.TMax = t_max;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene_tlas, RAY_FLAG_NONE, 0xffu, ray);
    query.Proceed();

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

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
    let grid_mask = grid_cell_in_bounds ? constants.light_grid[light_grid_cell_index(grid_cell)] : u32x4(0);

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

[[shader("raygeneration")]]
[[require(spvRayTracingKHR)]]
func rgs_main() -> void {
    let volume = probe_volumes[constants.volume_index];
    let ray_index = DispatchRaysIndex().x;
    let probe_index = DispatchRaysIndex().y;
    if (ray_index >= constants.rays_per_probe || probe_index >= volume.probe_count) {
        return;
    }

    if (!probe_updates_this_frame(probe_states[volume.probe_offset + probe_index].flags)) {
        return;
    }

    let origin = probe_world_position(volume, probe_states, probe_index);
    let direction = probe_ray_direction(ray_index, constants.rays_per_probe, constants.frame_index);

    let max_ray_distance = max(
        constants.max_ray_distance * f32(1u << volume.cascade_index),
        volume.max_probe_distance
    );

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = max_ray_distance;

    ProbeRayPayload payload = {};
    payload.radiance = f32x3(0.0);
    payload.hit_distance = max_ray_distance;

    TraceRay(scene_tlas, RAY_FLAG_FORCE_OPAQUE, 0xffu, 0, 0, 0, ray, payload);

    var radiance = clamp(com::sanitize(payload.radiance), 0.0, com::HALF_MAX);

    let ray_luminance = com::luminance(radiance);
    if (ray_luminance > constants.max_ray_radiance) {
        radiance *= constants.max_ray_radiance / ray_luminance;
    }

    u32 ray_data_width, ray_data_height;
    ray_data.GetDimensions(ray_data_width, ray_data_height);
    let ray_origin = ray_data_row_origin(volume.probe_offset + probe_index, constants.rays_per_probe, ray_data_width);

    ray_data[ray_origin + u32x2(ray_index, 0)] = f32x4(radiance, payload.hit_distance);
}

[[shader("miss")]]
[[require(spvRayTracingKHR)]]
func miss_main(inout ProbeRayPayload payload) -> void {
    payload.radiance = miss_radiance(
        constants.atmosphere,
        WorldRayOrigin(),
        WorldRayDirection(),
        constants.sun_direction,
        constants.ambient_color
    );
}

[[shader("closesthit")]]
[[require(spvRayTracingKHR, spvRayQueryKHR)]]
func closesthit_main(inout ProbeRayPayload payload, in BuiltInTriangleIntersectionAttributes attributes) -> void {
    let hit_distance = RayTCurrent();
    if (HitKind() != HIT_KIND_TRIANGLE_FRONT_FACE) {
        payload.hit_distance = -hit_distance;
        return;
    }

    payload.hit_distance = hit_distance;

    let volume = probe_volumes[constants.volume_index];
    let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
    let direction = WorldRayDirection();

    let use_radiance_cache = constants.distance_culling_enabled != 0 &&
                             constants.bounce_valid != 0 &&
                             hit_distance >= volume.max_probe_distance;
    if (use_radiance_cache) {
        let triangle = resolve_ray_triangle(geometry, InstanceID(), PrimitiveIndex());
        if (!triangle.valid) {
            return;
        }

        let hit_position = WorldRayOrigin() + direction * hit_distance;
        let face_normal = triangle.face_normal();

        ProbeSampleParams params;
        params.world_position = hit_position;
        params.normal = face_normal;
        params.geometric_normal = face_normal;
        params.view_direction = -direction;
        params.view_distance = hit_distance;
        params.specular_direction = face_normal;
        params.want_specular = false;
        params.normal_bias = constants.normal_bias;
        params.view_bias = constants.view_bias;

        let sample = sample_ddgi_radiance(
            probe_volumes,
            probe_states,
            constants.volume_count,
            irradiance_atlas,
            distance_atlas,
            linear_clamp_sampler,
            constants.radiance_atlas_y_offset,
            params
        );
        payload.radiance = sample.radiance * sample.coverage;
        return;
    }

    let hit = resolve_ray_hit(
        geometry,
        InstanceID(),
        PrimitiveIndex(),
        attributes.barycentrics,
        true,
        HIT_TEXTURE_LOD
    );
    if (!hit.valid) {
        return;
    }

    payload.radiance = shade_hit(
        hit,
        constants.volume_count,
        -direction,
        hit_distance,
        constants.sun_direction,
        constants.sun_intensity,
        constants.shadow_ray_offset,
        constants.normal_bias,
        constants.view_bias,
        constants.light_count,
        constants.bounce_valid,
        constants.atmosphere
    );
}
