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

static const f32 HIT_TEXTURE_LOD = 3.0;

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

func shade_hit(
    in RayHit hit,
    u32 volume_count,
    f32x3 view_direction,
    f32x3 sun_direction,
    f32 sun_intensity,
    f32 normal_bias,
    f32 view_bias,
    u32 light_count,
    u32 bounce_valid,
    Atmosphere *atmosphere
) -> f32x3 {
    let N = hit.surface.normal;
    let diffuse_albedo = hit.surface.albedo.rgb * (1.0 - hit.surface.metallic);
    let shadow_origin = hit.position + hit.face_normal * normal_bias;

    var radiance = hit.surface.emissive;

    let NoL = saturate(dot(N, sun_direction));
    if ((scene_flags & SceneFlags::HasDirectionalLight) != 0 && NoL > 0.0) {
        if (!trace_occluded(shadow_origin, sun_direction, normal_bias, com::HALF_MAX)) {
            let illuminance = sun_illuminance(atmosphere, hit.position, sun_direction, sun_intensity);
            radiance += diffuse_albedo * Fd_Lambert() * illuminance * NoL;
        }
    }

    for (u32 light_index = 0; light_index < light_count; light_index++) {
        let light = lights[light_index];
        if (light.kind == LightKind::Point) {
            radiance += point_light_contribution(
                hit.position,
                N,
                view_direction,
                hit.surface.albedo.rgb,
                hit.surface.roughness,
                hit.surface.metallic,
                light.position,
                light
            );
        } else if (light.kind == LightKind::Spot) {
            radiance += calculate_spot_light_contribution(
                hit.position,
                N,
                view_direction,
                hit.surface.albedo.rgb,
                hit.surface.roughness,
                hit.surface.metallic,
                light.position,
                light.direction,
                light
            );
        }
    }

    if (bounce_valid != 0) {
        let bounce = sample_ddgi_irradiance(
            probe_volumes,
            volume_count,
            irradiance_atlas,
            distance_atlas,
            linear_clamp_sampler,
            hit.position,
            N,
            view_direction,
            normal_bias,
            view_bias
        );
        if (bounce.valid) {
            radiance += diffuse_albedo * bounce.irradiance;
        }
    }

    return radiance;
}

[[shader("compute")]]
[[require(spvRayQueryKHR)]]
[[numthreads(32, 1, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform Atmosphere *atmosphere,
    uniform u32 volume_index,
    uniform u32 volume_count,
    uniform u32 rays_per_probe,
    uniform u32 frame_index,
    uniform u32 light_count,
    uniform u32 bounce_valid,
    uniform f32 max_ray_distance,
    uniform f32 normal_bias,
    uniform f32 view_bias,
    uniform f32x3 sun_direction,
    uniform f32 sun_intensity,
    uniform f32x3 ambient_color,
    uniform f32 max_ray_radiance
) -> void {
    let volume = probe_volumes[volume_index];
    let ray_index = thread_id.x;
    let probe_index = thread_id.y;
    if (ray_index >= rays_per_probe || probe_index >= volume.probe_count) {
        return;
    }

    let origin = volume.probe_position(probe_index);
    let direction = probe_ray_direction(ray_index, rays_per_probe, frame_index);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = max_ray_distance;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene_tlas, RAY_FLAG_NONE, 0xffu, ray);
    query.Proceed();

    var radiance = f32x3(0.0);
    var hit_distance = max_ray_distance;

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        hit_distance = query.CommittedRayT();

        if (!query.CommittedTriangleFrontFace()) {
            // Backfaces mean the probe sits inside geometry along this ray. The distance is negated so
            // the update pass can tell those rays apart instead of trusting them.
            hit_distance = -hit_distance;
        } else {
            let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
            let hit = resolve_ray_hit(
                geometry,
                query.CommittedInstanceID(),
                query.CommittedPrimitiveIndex(),
                query.CommittedTriangleBarycentrics(),
                true,
                HIT_TEXTURE_LOD
            );

            if (hit.valid) {
                radiance = shade_hit(
                    hit,
                    volume_count,
                    -direction,
                    sun_direction,
                    sun_intensity,
                    normal_bias,
                    view_bias,
                    light_count,
                    bounce_valid,
                    atmosphere
                );
            }
        }
    } else {
        radiance = miss_radiance(atmosphere, origin, direction, sun_direction, ambient_color);
    }

    radiance = clamp(com::sanitize(radiance), 0.0, com::HALF_MAX);

    let ray_luminance = com::luminance(radiance);
    if (ray_luminance > max_ray_radiance) {
        radiance *= max_ray_radiance / ray_luminance;
    }

    ray_data[u32x2(ray_index, volume.probe_offset + probe_index)] = f32x4(radiance, hit_distance);
}
