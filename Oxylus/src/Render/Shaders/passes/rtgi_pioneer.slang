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

[[vk::binding(18, 0)]] RWTexture2D<f32x4> pioneer_hit_y;

[[shader("raygeneration")]]
[[require(spvRayTracingKHR)]]
func rgs_pioneer() -> void {
    let cell = DispatchRaysIndex().xy;

    u32 half_width;
    u32 half_height;
    half_res_depth.GetDimensions(half_width, half_height);
    let half_size = u32x2(half_width, half_height);

    let half_pixel = rtgi_pioneer_pixel(cell, constants.frame_index);
    if (any(half_pixel >= half_size)) {
        pioneer_hit_y[cell] = f32x4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    let load = u32x3(half_pixel, 0u);
    let depth = half_res_depth.Load(load);
    if (depth == 0.0) {
        pioneer_hit_y[cell] = f32x4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    let uv = f32x2(half_pixel * 2u + 1u) / camera.resolution;
    let world_position = camera.world_position_from_linear_depth(uv, depth);
    let face_normal = com::decompress_normal_32(half_res_face_normal.Load(load));
    let pixel_width = rtgi_pixel_width_ws(camera, uv, depth);
    let primary_ray = normalize(world_position - camera.position.xyz);

    com::rand_seed(constants.frame_index * 257u + half_pixel.x * 9629u + half_pixel.y * 10069u + 17u);

    let cos_theta = com::rand();
    let sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    let phi = com::rand() * 2.0 * PI;
    let local_direction = f32x3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
    let direction = com::build_tangent_basis(face_normal).to_world(local_direction);

    RayDesc ray;
    ray.Origin = rtgi_calc_ray_start(world_position, face_normal, primary_ray) - primary_ray * pixel_width;
    ray.Direction = direction;
    ray.TMin = pixel_width * 0.5;
    ray.TMax = RTGI_PIONEER_MAX_DISTANCE;

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

    // a sky miss reports RTGI_RAY_MISS_T, far past TMax, and would overflow the position
    let hit_position = ray.Origin + direction * min(payload.t, ray.TMax);
    let brightness = com::rgb_to_ycocg(clamp(com::sanitize(radiance), 0.0, com::HALF_MAX)).x;
    pioneer_hit_y[cell] = f32x4(hit_position, max(brightness, 0.0));
}
