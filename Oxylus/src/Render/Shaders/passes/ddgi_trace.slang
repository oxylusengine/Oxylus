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

[[vk::binding(10, 0)]] RWTexture2D<f32x4, 1, ImageFormat.RGBA16F> ray_data;

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
    LightGridCell *light_grid;
    VSMPointSpotView *pointspot_views;
    u32 radiance_atlas_y_offset;
    u32 distance_culling_enabled;
    f32x3 light_grid_origin;
};

[[vk::push_constant]] ConstantBuffer<TraceConstants> constants;

#include <passes/ddgi_trace_common.slang>

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

[[shader("closesthit")]]
[[require(spvRayTracingKHR, spvRayQueryKHR)]]
func closesthit_main(inout ProbeRayPayload payload, in BuiltInTriangleIntersectionAttributes attributes) -> void {
    let hit_distance = RayTCurrent();
    let is_front_face = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    if (!record_probe_hit_distance(payload, InstanceID(), PrimitiveIndex(), hit_distance, is_front_face)) {
        return;
    }

    let volume = probe_volumes[constants.volume_index];
    let direction = WorldRayDirection();
    let probe_flags = probe_states[volume.probe_offset + DispatchRaysIndex().y].flags;
    let request_mode = select(probe_is_requested_directly(probe_flags), PROBE_REQUEST_INDIRECT, PROBE_REQUEST_NONE);

    let use_radiance_cache = constants.distance_culling_enabled != 0 &&
                             constants.bounce_valid != 0 &&
                             hit_distance >= volume.max_probe_distance;
    if (use_radiance_cache) {
        let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
        let triangle = resolve_ray_triangle(geometry, InstanceID(), PrimitiveIndex());
        if (!triangle.valid) {
            return;
        }

        let hit_position = WorldRayOrigin() + direction * hit_distance;
        let face_normal = (is_front_face ? 1.0 : -1.0) * triangle.face_normal();

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
            params,
            probe_requests,
            request_mode
        );
        payload.radiance = sample.radiance * sample.coverage;
        return;
    }

    shade_probe_hit(
        payload,
        InstanceID(),
        PrimitiveIndex(),
        attributes.barycentrics,
        hit_distance,
        direction,
        is_front_face,
        request_mode
    );
}
