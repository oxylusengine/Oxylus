import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] Image2D<f32x4> ray_data;
[[vk::binding(2, 0)]] RWStructuredBuffer<ProbeState> probe_states;

// x^2 / spacing.x^2 + y^2 / spacing.y^2 + z^2 / spacing.z^2 < 0.45^2, so a probe never crosses into its
// neighbour's cell and breaks the trilinear weights that assume a regular grid.
static const f32 MAX_OFFSET_RATIO = 0.45;
static const f32 MAX_OFFSET_RATIO_SQUARED = MAX_OFFSET_RATIO * MAX_OFFSET_RATIO;
static const f32 RELOCATE_MAX_STEP_RATIO = MAX_OFFSET_RATIO * 0.5;
static const f32 RELOCATE_BACKOFF_HYSTERESIS = 2.0;
static const f32 RELOCATE_BACKOFF_RATE = 0.25;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform u32 volume_index,
    uniform u32 rays_per_probe,
    uniform u32 frame_index,
    uniform f32 min_frontface_distance
) -> void {
    let volume = probe_volumes[volume_index];
    let probe_index = thread_id.x;
    if (probe_index >= volume.probe_count) {
        return;
    }

    let probe_row = volume.probe_offset + probe_index;

    let flags = probe_states[probe_row].flags;
    if (!probe_updates_this_frame(flags)) {
        return;
    }

    var offset = probe_states[probe_row].offset;

    var closest_backface_index = -1;
    var closest_frontface_index = -1;
    var farthest_frontface_index = -1;
    var closest_backface_distance = com::HALF_MAX;
    var closest_frontface_distance = com::HALF_MAX;
    var farthest_frontface_distance = 0.0f;
    var backface_count = 0.0f;

    for (u32 ray_index = 0; ray_index < rays_per_probe; ray_index++) {
        var distance = ray_data.Load(u32x3(ray_index, probe_row, 0)).a;

        if (distance < 0.0) {
            // backface
            backface_count += 1.0;
            distance = -distance;
            if (distance < closest_backface_distance) {
                closest_backface_distance = distance;
                closest_backface_index = i32(ray_index);
            }
        } else {
            // frontface
            if (distance < closest_frontface_distance) {
                closest_frontface_distance = distance;
                closest_frontface_index = i32(ray_index);
            }
            if (distance > farthest_frontface_distance) {
                farthest_frontface_distance = distance;
                farthest_frontface_index = i32(ray_index);
            }
        }
    }

    var full_offset = f32x3(com::HALF_MAX);

    let cascade_scale = f32(1u << volume.cascade_index);
    let min_distance = min_frontface_distance * cascade_scale;
    let max_step = RELOCATE_MAX_STEP_RATIO * min(min(volume.spacing.x, volume.spacing.y), volume.spacing.z);

    let backface_ratio = backface_count / f32(rays_per_probe);
    let was_inside = probe_is_inactive(flags);
    let inside_geometry = closest_backface_index != -1 &&
                          backface_ratio > (was_inside ? PROBE_ESCAPED_BACKFACE_RATIO : PROBE_INSIDE_BACKFACE_RATIO);

    if (inside_geometry) {
        let closest_backface_direction = probe_ray_direction(u32(closest_backface_index), rays_per_probe, frame_index);
        full_offset = offset + closest_backface_direction * (closest_backface_distance + min_distance * 0.5);
    } else if (closest_frontface_distance < min_distance && farthest_frontface_index != -1) {
        let closest_frontface_direction = probe_ray_direction(u32(closest_frontface_index), rays_per_probe, frame_index);
        var farthest_frontface_direction = probe_ray_direction(
            u32(farthest_frontface_index),
            rays_per_probe,
            frame_index
        );

        if (dot(closest_frontface_direction, farthest_frontface_direction) <= 0.0) {
            farthest_frontface_direction *= min(farthest_frontface_distance, max_step);
            full_offset = offset + farthest_frontface_direction;
        }
    } else if (closest_frontface_distance > min_distance * RELOCATE_BACKOFF_HYSTERESIS) {
        let offset_length = length(offset);
        if (offset_length > 1e-4) {
            let move_back_margin = min(closest_frontface_distance - min_distance, offset_length) *
                                   RELOCATE_BACKOFF_RATE;
            full_offset = offset - offset * (move_back_margin / offset_length);
        }
    }

    let normalized_offset = full_offset / volume.spacing;
    if (dot(normalized_offset, normalized_offset) < MAX_OFFSET_RATIO_SQUARED) {
        offset = full_offset;
    }

    ProbeState state;
    state.offset = com::sanitize(offset);
    state.flags = probe_pack_flags(inside_geometry, probe_updates_this_frame(flags), probe_is_reset(flags));

    probe_states[probe_row] = state;
}
