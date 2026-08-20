import common;
import gpu;
import ddgi;

[[vk::binding(0, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(1, 0)]] Image2D<f32x4> ray_data;
[[vk::binding(2, 0)]] RWStructuredBuffer<ProbeState> probe_states;

// A probe may wander at most this fraction of a grid cell, so it never crosses into its neighbour's
// cell and breaks the trilinear weights that assume a regular grid.
static const f32 MAX_OFFSET_RATIO = 0.45;
// Clearance is only ever measured to within one ray of the sphere's spacing, and a small feature is
// hit some frames and missed others. Acting on differences smaller than this band would just chase
// that noise.
static const f32 CLEARANCE_DEADBAND = 0.1;
// Retreat at once, return slowly. A probe that only occasionally sees the edge it is next to should
// keep the clearance it earned on the frames it did see it, rather than being pulled back in between.
static const f32 RETURN_RATE = 0.05;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32x3 thread_id: SV_DispatchThreadID,
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

    var backface_count = 0u;
    var closest_backface_distance = com::HALF_MAX;
    var closest_backface_direction = f32x3(0.0);
    var closest_frontface_distance = com::HALF_MAX;
    var closest_frontface_direction = f32x3(0.0);
    var crowding = f32x3(0.0);

    for (u32 ray_index = 0; ray_index < rays_per_probe; ray_index++) {
        let distance = ray_data.Load(u32x3(ray_index, probe_row, 0)).a;
        let direction = probe_ray_direction(ray_index, rays_per_probe, frame_index);

        if (distance < 0.0) {
            backface_count += 1;
            if (-distance < closest_backface_distance) {
                closest_backface_distance = -distance;
                closest_backface_direction = direction;
            }

            continue;
        }

        if (distance < closest_frontface_distance) {
            closest_frontface_distance = distance;
            closest_frontface_direction = direction;
        }
        if (distance < min_frontface_distance) {
            crowding += direction * (min_frontface_distance - distance);
        }
    }

    let backface_ratio = f32(backface_count) / f32(rays_per_probe);
    let was_inside = probe_is_inactive(flags);
    let inside_geometry = backface_ratio > (was_inside ? PROBE_ESCAPED_BACKFACE_RATIO : PROBE_INSIDE_BACKFACE_RATIO);

    let current_offset = probe_states[probe_row].offset;
    var full_offset = current_offset;

    if (inside_geometry) {
        full_offset += closest_backface_direction * (closest_backface_distance + min_frontface_distance * 0.5);
    } else if (closest_frontface_distance < min_frontface_distance * (1.0 - CLEARANCE_DEADBAND)) {
        let crowding_length = length(crowding);
        let escape_direction = crowding_length > 1e-6 ? crowding / crowding_length : closest_frontface_direction;

        full_offset -= escape_direction * (min_frontface_distance - closest_frontface_distance);
    } else if (closest_frontface_distance > min_frontface_distance * (1.0 + CLEARANCE_DEADBAND)) {
        let travelled = length(current_offset);
        if (travelled > 0.0) {
            let margin = min(closest_frontface_distance - min_frontface_distance, travelled);
            full_offset -= normalize(current_offset) * margin * RETURN_RATE;
        }
    }

    let accepted = all(abs(full_offset) < MAX_OFFSET_RATIO * volume.spacing);

    ProbeState state;
    state.offset = accepted ? com::sanitize(full_offset) : current_offset;
    state.flags = probe_pack_flags(
        inside_geometry,
        probe_updates_this_frame(flags),
        probe_is_reset(flags),
        probe_update_interval(flags)
    );

    probe_states[probe_row] = state;
}
