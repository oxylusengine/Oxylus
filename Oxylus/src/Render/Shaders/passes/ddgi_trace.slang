import common;
import gpu;
import scene;
import hit;
import pbr;
import sky;
import ddgi;

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
};

[[vk::push_constant]] ConstantBuffer<TraceConstants> constants;

struct ProbeRayPayload {
    f32 hit_distance;
    u32 mesh_instance_index;
    u32 primitive_index;
    f32x2 barycentrics;
    bool front_face;
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

func punctual_diffuse(
    in Light light,
    f32x3 position,
    f32x3 normal,
    f32x3 diffuse_albedo,
    f32x3 shadow_origin,
    f32 shadow_ray_offset
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

    if (trace_occluded(shadow_origin, L, shadow_ray_offset, max(light_distance - shadow_ray_offset, 0.0))) {
        return f32x3(0.0);
    }

    return diffuse_albedo * Fd_Lambert() * light.color * light.intensity * attenuation * NoL;
}

func shade_hit(
    in RayHit hit,
    u32 volume_count,
    f32x3 view_direction,
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

    for (u32 light_index = 0; light_index < light_count; light_index++) {
        let light = lights[light_index];
        if (light.kind == LightKind::Point || light.kind == LightKind::Spot) {
            radiance += punctual_diffuse(light, hit.position, N, diffuse_albedo, shadow_origin, shadow_ray_offset);
        }
    }

    if (bounce_valid != 0) {
        let bounce = sample_ddgi_irradiance(
            probe_volumes,
            probe_states,
            volume_count,
            irradiance_atlas,
            distance_atlas,
            linear_clamp_sampler,
            hit.position,
            N,
            view_direction,
            N,
            false,
            normal_bias,
            view_bias
        );
        radiance += diffuse_albedo * bounce.irradiance * bounce.coverage;
    }

    return radiance;
}

[[shader("raygeneration")]]
[[require(spvRayTracingKHR, spvRayQueryKHR)]]
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
    payload.hit_distance = -1.0;

    TraceRay(scene_tlas, RAY_FLAG_FORCE_OPAQUE, 0xffu, 0, 0, 0, ray, payload);

    var radiance = f32x3(0.0);
    var hit_distance = max_ray_distance;

    if (payload.hit_distance < 0.0) {
        radiance = miss_radiance(
            constants.atmosphere,
            origin,
            direction,
            constants.sun_direction,
            constants.ambient_color
        );
    } else if (!payload.front_face) {
        hit_distance = -payload.hit_distance;
    } else {
        hit_distance = payload.hit_distance;

        let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
        let hit = resolve_ray_hit(
            geometry,
            payload.mesh_instance_index,
            payload.primitive_index,
            payload.barycentrics,
            true,
            HIT_TEXTURE_LOD
        );

        if (hit.valid) {
            radiance = shade_hit(
                hit,
                constants.volume_count,
                -direction,
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
    }

    radiance = clamp(com::sanitize(radiance), 0.0, com::HALF_MAX);

    let ray_luminance = com::luminance(radiance);
    if (ray_luminance > constants.max_ray_radiance) {
        radiance *= constants.max_ray_radiance / ray_luminance;
    }

    u32 ray_data_width, ray_data_height;
    ray_data.GetDimensions(ray_data_width, ray_data_height);
    let ray_origin = ray_data_row_origin(volume.probe_offset + probe_index, constants.rays_per_probe, ray_data_width);

    ray_data[ray_origin + u32x2(ray_index, 0)] = f32x4(radiance, hit_distance);
}

[[shader("miss")]]
[[require(spvRayTracingKHR)]]
func miss_main(inout ProbeRayPayload payload) -> void {
    payload.hit_distance = -1.0;
}

[[shader("closesthit")]]
[[require(spvRayTracingKHR)]]
func closesthit_main(inout ProbeRayPayload payload, in BuiltInTriangleIntersectionAttributes attributes) -> void {
    payload.hit_distance = RayTCurrent();
    payload.mesh_instance_index = InstanceID();
    payload.primitive_index = PrimitiveIndex();
    payload.barycentrics = attributes.barycentrics;
    payload.front_face = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
}
