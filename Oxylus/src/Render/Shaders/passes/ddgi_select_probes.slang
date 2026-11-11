import common;
import gpu;
import scene;
import cull;
import ddgi;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(2, 0)]] RWStructuredBuffer<ProbeState> probe_states;
[[vk::binding(3, 0)]] RWStructuredBuffer<u32> probe_update_list;
[[vk::binding(4, 0)]] RWStructuredBuffer<ProbeUpdateArgs> probe_update_args;
[[vk::binding(5, 0)]] RWStructuredBuffer<u32> probe_requests;

// the probe just snapped from far edge previous frame to where its in current frame
// force reset it so it doesnt blend with invalid history data anymore
func probe_scrolled_in(in ProbeVolume volume, u32x3 grid, i32x3 scroll_delta) -> bool {
    var is_scrolled_in = false;
    for (u32 axis = 0; axis < 3; axis++) {
        let delta = scroll_delta[axis];
        let count = i32(volume.counts[axis]);
        let coord = i32(grid[axis]);

        if (abs(delta) >= count) {
            is_scrolled_in = true;
        } else if (delta > 0) {
            is_scrolled_in = is_scrolled_in || coord >= count - delta;
        } else if (delta < 0) {
            is_scrolled_in = is_scrolled_in || coord < -delta;
        }
    }

    return is_scrolled_in;
}

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform u32 volume_index,
    uniform u32 frame_index,
    uniform u32 max_interval,
    uniform f32 full_rate_distance,
    uniform u32 update_all,
    uniform u32 force_update_all,
    uniform u32 distance_culling_enabled,
    uniform u32 requests_enabled,
    uniform i32x3 scroll_delta
) -> void {
    let volume = probe_volumes[volume_index];
    let probe_index = thread_id.x;
    if (probe_index >= volume.probe_count) {
        return;
    }

    let probe_row = volume.probe_offset + probe_index;
    let flags = probe_states[probe_row].flags;

    let grid = volume.grid_coord(probe_index);
    let reset = or(update_all != 0, probe_scrolled_in(volume, grid, scroll_delta));
    if (reset) {
        probe_states[probe_row].offset = f32x3(0.0);
    }

    let request_mask = PROBE_REQUEST_TIMER_MASK | (PROBE_REQUEST_TIMER_MASK << PROBE_REQUEST_INDIRECT_SHIFT);
    let word = probe_requests[probe_row];
    let direct_timer = word & PROBE_REQUEST_TIMER_MASK;
    let indirect_timer = (word >> PROBE_REQUEST_INDIRECT_SHIFT) & PROBE_REQUEST_TIMER_MASK;
    let decayed = (direct_timer - min(direct_timer, 1u)) |
                  ((indirect_timer - min(indirect_timer, 1u)) << PROBE_REQUEST_INDIRECT_SHIFT);
    probe_requests[probe_row] = select(reset, 0u, decayed);

    // a probe is requested through any of the eight cells it is a corner of
    var is_requested = and(!reset, (word & request_mask) != 0);
    var is_requested_directly = and(!reset, direct_timer != 0);
    for (u32 corner = 1; corner < 8; corner++) {
        let cell = i32x3(grid) - i32x3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        if (any(cell < 0)) {
            continue;
        }

        if (or(update_all != 0, probe_scrolled_in(volume, u32x3(cell), scroll_delta))) {
            continue;
        }

        let cell_word = probe_requests[volume.probe_offset + volume.probe_index_at(u32x3(cell))];
        is_requested = or(is_requested, (cell_word & request_mask) != 0);
        is_requested_directly = or(is_requested_directly, (cell_word & PROBE_REQUEST_TIMER_MASK) != 0);
    }

    let is_requests_disabled = requests_enabled == 0;
    is_requested = or(is_requested, is_requests_disabled);
    is_requested_directly = or(is_requested_directly, is_requests_disabled);

    let needs_first_trace = or(reset, probe_is_untraced(flags));
    let is_waking = and(is_requested, !probe_is_requested(flags));

    let position = volume.probe_position(probe_index) + probe_states[probe_row].offset;
    let view_distance = length(position - camera.position.xyz);
    let distance_interval = clamp(u32(view_distance / max(full_rate_distance, 1e-3)), 1u, max_interval);
    let is_rated = or(!is_requests_disabled, test_frustum(camera.projection_view, position, volume.spacing * 0.5));
    let interval = select(is_rated, distance_interval, max_interval);

    let mesh_distance_culled = distance_culling_enabled != 0 &&
                               probe_is_mesh_distant(flags) &&
                               !probe_is_inside_geometry(flags);
    let update_interval = mesh_distance_culled ? PROBE_MESH_DISTANCE_RECHECK_INTERVAL : interval;
    let is_due = ((frame_index + probe_index) % update_interval) == 0;
    let update = and(is_requested, or(or(needs_first_trace, is_waking), or(force_update_all != 0, is_due)));
    let untraced = and(needs_first_trace, !update);

    probe_states[probe_row].flags = probe_pack_flags(
        !reset && probe_is_inside_geometry(flags),
        !reset && probe_is_mesh_distant(flags),
        update,
        and(update, needs_first_trace),
        untraced,
        is_requested,
        is_requested_directly,
        !reset ? probe_mesh_distance_grace(flags) : 0u,
        !reset ? probe_inside_votes(flags) : 0u,
        update_interval
    );

    if (update) {
        let slot = __atomic_add(probe_update_args[0].count, 1u, MemoryOrder::Relaxed);
        probe_update_list[slot] = probe_pack_update_entry(volume_index, probe_index);
    }
}
