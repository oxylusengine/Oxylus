import common;
import gpu;
import scene;
import hit;
import pbr;
import sky;
import ddgi;
import rmvsm;
import embedded.possion;
import rtgi_common;

#include <passes/rtgi_rt_common.slang>
#include <passes/rtgi_guided_sampling.slang>

[[vk::binding(18, 0)]] StructuredBuffer<RTGIRayEntry> ray_list;
[[vk::binding(19, 0)]] StructuredBuffer<RTGIRayCounters> counters;
[[vk::binding(20, 0)]] RWStructuredBuffer<RTGIRayResult> ray_result;
[[vk::binding(21, 0)]] Texture2D<u32> sample_count_image;
[[vk::binding(22, 0)]] Texture2D<f32x4> pioneer_guide_hit_y;

static const u32 RTGI_SEED_HISTORY = 6151u;
static const u32 RTGI_GUIDE_FETCH_ATTEMPTS = 3u;
static const f32 RTGI_GUIDE_FETCH_CELL_RADIUS = 4.0;

func rtgi_guide_fetch_pixel_hash(u32x2 pixel) -> u32 {
    let state = (pixel.x * 9629u ^ pixel.y * 10069u ^ 0x2545F491u) * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// reconnects the hit picked by a nearby pioneer cell to this pixel, does not touch rand()
func rtgi_fetch_ray_guide(
    u32x2 pixel,
    f32x3 world_position,
    f32 depth,
    f32x3 face_normal,
    f32 pixel_width,
    u32 ray_key,
    out f32x3 guide_direction,
    out f32 guide_brightness
) -> bool {
    guide_direction = f32x3(0.0);
    guide_brightness = 0.0;

    u32 half_width;
    u32 half_height;
    half_res_depth.GetDimensions(half_width, half_height);
    let half_size = u32x2(half_width, half_height);
    let half_size_f = f32x2(half_size);

    let rotation = i32x2(rtgi_pioneer_rotation(constants.frame_index));
    let grid_size = i32x2(rtgi_pioneer_grid_size(half_size));
    let own_cell_pos = (f32x2(i32x2(pixel) - rotation) + 0.5) / f32(RTGI_PIONEER_GRID_DIV);

    let pixel_hash = rtgi_guide_fetch_pixel_hash(pixel);
    let kernel_angle = f32(pixel_hash >> 8u) * (6.28318530718 / 16777216.0);
    let kernel_sin = sin(kernel_angle);
    let kernel_cos = cos(kernel_angle);

    for (u32 attempt = 0u; attempt < RTGI_GUIDE_FETCH_ATTEMPTS; attempt++) {
        let poisson = POSSION_16[(ray_key * RTGI_GUIDE_FETCH_ATTEMPTS + attempt) & 15u].xy;
        let tap = f32x2(kernel_cos * poisson.x - kernel_sin * poisson.y, kernel_sin * poisson.x + kernel_cos * poisson.y);
        let cell = clamp(i32x2(floor(own_cell_pos + tap * RTGI_GUIDE_FETCH_CELL_RADIUS)), i32x2(0), grid_size - 1);
        let ref_pixel = cell * i32(RTGI_PIONEER_GRID_DIV) + rotation;
        if (or(any(ref_pixel < 0), any(ref_pixel >= i32x2(half_size)))) {
            continue;
        }

        let ref_depth = half_res_depth.Load(i32x3(ref_pixel, 0));
        if (ref_depth == 0.0) {
            continue;
        }

        let hit_y = pioneer_guide_hit_y.Load(i32x3(cell, 0));
        let ref_uv = (f32x2(ref_pixel) + 0.5) / half_size_f;
        let ref_position = camera.world_position_from_linear_depth(ref_uv, ref_depth);
        let ref_pixel_width = rtgi_pixel_width_ws(camera, ref_uv, ref_depth);
        let is_near = distance(ref_position, world_position) <=
                      RTGI_GUIDE_RESAMPLE_PX_DIST_THRESHOLD * min(pixel_width, ref_pixel_width);

        let to_hit = hit_y.xyz - world_position;
        let hit_distance = length(to_hit);
        let direction = to_hit / max(hit_distance, 1.0e-4);
        let is_reachable = and(hit_distance >= 1.0e-4, dot(face_normal, direction) > 0.0);
        if (and(and(hit_y.w > 0.0, is_near), is_reachable)) {
            guide_direction = direction;
            guide_brightness = hit_y.w;
            return true;
        }
    }

    return false;
}

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

    var direction = f32x3(0.0);
    var guide_weight = 1.0;
    var is_guided = false;
    if (constants.is_guiding_enabled != 0) {
        f32x3 guide_direction;
        f32 guide_brightness;
        if (rtgi_fetch_ray_guide(
                half_pixel, world_position, depth, face_normal, pixel_width, entry.sample_index,
                guide_direction, guide_brightness)) {
            direction = rtgi_sample_guided_diffuse_dir(
                face_normal, guide_direction, constants.guide_concentration, guide_weight);
            is_guided = true;
        }
    }

    // both samplers draw two rand() values, the per ray skip above relies on that so only one of them may run
    if (!is_guided) {
        direction = com::build_tangent_basis(face_normal).to_world(com::rand_cosine_sample_hemi());
    }

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
    result.radiance = clamp(com::sanitize(radiance * guide_weight), 0.0, com::HALF_MAX) * RTGI_RADIANCE_SCALE;
    result.t = payload.t;
    result.packed_dir = com::compress_normal_32(direction);
    ray_result[ray_index] = result;
}
