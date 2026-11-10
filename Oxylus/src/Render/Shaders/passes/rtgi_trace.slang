import common;
import gpu;
import scene;
import hit;
import pbr;
import sky;
import ddgi;
import rmvsm;
import rtgi_common;

#include <passes/rtgi_rt_common.slang>

[[vk::binding(18, 0)]] StructuredBuffer<RTGIRayEntry> ray_list;
[[vk::binding(19, 0)]] StructuredBuffer<RTGIRayCounters> counters;
[[vk::binding(20, 0)]] RWStructuredBuffer<RTGIRayResult> ray_result;
[[vk::binding(21, 0)]] Texture2D<u32> sample_count_image;

static const u32 RTGI_SEED_HISTORY = 6151u;

[[shader("raygeneration")]]
[[require(spvRayTracingKHR)]]
func rgs_main() -> void {
    let ray_index = DispatchRaysIndex().y * 128u + DispatchRaysIndex().x;
    if (ray_index >= min(counters[0].ray_list_count, constants.ray_list_limit)) {
        return;
    }

    let entry = ray_list[ray_index];
    let half_pixel = u32x2(entry.packed_xy & 0xffffu, entry.packed_xy >> 16u);
    let load = u32x3(half_pixel, 0u);

    let depth = half_res_depth.Load(load);
    if (depth == 0.0) {
        RTGIRayResult sky_result;
        sky_result.radiance = f32x3(0.0);
        sky_result.t = 0.0;
        sky_result.packed_dir = 0u;
        ray_result[ray_index] = sky_result;
        return;
    }

    let uv = f32x2(half_pixel * 2u + 1u) / camera.resolution;
    let world_position = camera.world_position_from_linear_depth(uv, depth);
    let face_normal = com::decompress_normal_32(half_res_face_normal.Load(load));
    let pixel_width = rtgi_pixel_width_ws(camera, uv, depth);
    let primary_ray = normalize(world_position - camera.position.xyz);

    // the history count advances with the rays a pixel shot, it keeps repeated frames from drawing the same directions
    let history_count = rtgi_unpack_normal_count(sample_count_image.Load(load));
    let history_seed = u32(max(history_count, 0.0)) * RTGI_SEED_HISTORY;
    com::rand_seed(constants.frame_index * 257u + history_seed + half_pixel.x * 9629u + half_pixel.y * 10069u);
    for (u32 skip = 0u; skip < entry.sample_index * 2u; skip++) {
        com::rand();
    }

    let tangent = normalize(cross(face_normal, f32x3(0.0001, 0.0001, 1.0001)));
    let bitangent = cross(tangent, face_normal);
    let local_direction = com::rand_cosine_sample_hemi();
    let direction = tangent * local_direction.x + bitangent * local_direction.y + face_normal * local_direction.z;

    RayDesc ray;
    ray.Origin = rtgi_calc_ray_start(world_position, face_normal, primary_ray) - primary_ray * pixel_width;
    ray.Direction = direction;
    ray.TMin = pixel_width * 0.5;
    ray.TMax = constants.max_distance;

    RTGIPayload payload = {};
    payload.t = RTGI_RAY_MISS_T;
    payload.instance_id = RTGI_NO_HIT;
    TraceRay(scene_tlas, RAY_FLAG_NONE, TLAS_MASK_GEOMETRY, 0, 0, 0, ray, payload);

    var radiance = f32x3(0.0);
    if (payload.instance_id == RTGI_NO_HIT) {
        radiance = miss_radiance(
            constants.atmosphere,
            ray.Origin,
            direction,
            constants.sun_direction,
            constants.ambient_color
        );
    } else {
        radiance = shade_ray_hit(payload, ray.Origin, direction);
    }

    RTGIRayResult result;
    result.radiance = clamp(com::sanitize(radiance), 0.0, com::HALF_MAX) * RTGI_RADIANCE_SCALE;
    result.t = payload.t;
    result.packed_dir = com::compress_normal_32(direction);
    ray_result[ray_index] = result;
}
