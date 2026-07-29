import common;
import gpu;
import scene;

[[vk::binding(0)]] StructuredBuffer<Light> lights;
[[vk::binding(1)]] RWStructuredBuffer<u32x4> light_grid;

// Cone vs sphere test by Bart Wronski,
// https://bartwronski.com/2017/04/13/cull-that-cone/
func cone_intersects_sphere(f32x3 apex, f32x3 axis, f32 range, f32 angle, f32x3 sphere_center, f32 sphere_radius) -> bool {
    let V = sphere_center - apex;
    let V_len_sq = dot(V, V);
    let V1_len = dot(V, axis);
    let distance_closest_point = cos(angle) * sqrt(max(V_len_sq - V1_len * V1_len, 0.0)) - V1_len * sin(angle);

    let angle_cull = distance_closest_point > sphere_radius;
    let front_cull = V1_len > sphere_radius + range;
    let back_cull = V1_len < -sphere_radius;
    return !(angle_cull || front_cull || back_cull);
}

func aabb_dist_sq_to_point(f32x3 aabb_min, f32x3 aabb_max, f32x3 point) -> f32 {
    let closest = clamp(point, aabb_min, aabb_max);
    let d = point - closest;
    return dot(d, d);
}

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32 thread_id : SV_DispatchThreadID,
    uniform u32 light_count,
    uniform f32x3 grid_origin,
) -> void {
    if (thread_id >= LIGHT_GRID_CELL_COUNT) {
        return;
    }

    let cell = i32x3(
        i32(thread_id) % LIGHT_GRID_RESOLUTION.x,
        (i32(thread_id) / LIGHT_GRID_RESOLUTION.x) % LIGHT_GRID_RESOLUTION.y,
        i32(thread_id) / (LIGHT_GRID_RESOLUTION.x * LIGHT_GRID_RESOLUTION.y)
    );

    let cell_min = grid_origin + f32x3(cell) * LIGHT_GRID_CELL_SIZE;
    let cell_max = cell_min + LIGHT_GRID_CELL_SIZE;
    let cell_center = (cell_min + cell_max) * 0.5;
    let cell_radius = LIGHT_GRID_CELL_SIZE * 0.5 * sqrt(3.0);

    var mask = u32x4(0);
    for (var light_index = 0u; light_index < light_count; light_index++) {
        let light = lights[light_index];
        if (light.shadow_map_index < 0 || light.range <= 0.0) {
            continue;
        }

        var affects_cell = aabb_dist_sq_to_point(cell_min, cell_max, light.position) <= light.range * light.range;
        if (affects_cell && light.kind == LightKind::Spot) {
            affects_cell = cone_intersects_sphere(
                light.position,
                normalize(light.direction),
                light.range,
                light.outer_cone_angle,
                cell_center,
                cell_radius
            );
        }

        if (affects_cell) {
            mask[light_grid_mask_word(light.kind, light.shadow_map_index)] |= light_grid_mask_bit(light.shadow_map_index);
        }
    }

    light_grid[thread_id] = mask;
}
