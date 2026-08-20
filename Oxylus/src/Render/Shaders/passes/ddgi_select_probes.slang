import common;
import gpu;
import scene;
import cull;
import ddgi;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(2, 0)]] RWStructuredBuffer<ProbeState> probe_states;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32x3 thread_id: SV_DispatchThreadID,
    uniform u32 volume_index,
    uniform u32 frame_index,
    uniform u32 max_interval,
    uniform f32 full_rate_distance,
    uniform u32 update_all,
    uniform i32x3 scroll_delta
) -> void {
    let volume = probe_volumes[volume_index];
    let probe_index = thread_id.x;
    if (probe_index >= volume.probe_count) {
        return;
    }

    let probe_row = volume.probe_offset + probe_index;
    let flags = probe_states[probe_row].flags;

    // A probe whose slot wrapped in from the far side of the grid this frame is sitting somewhere it has
    // never been. Its irradiance, its distance moments and its relocation offset all describe the cell
    // that dropped off the opposite edge, so it starts over instead of blending onto them.
    let grid = volume.grid_coord(probe_index);
    var reset = update_all != 0;
    for (u32 axis = 0; axis < 3; axis++) {
        let delta = scroll_delta[axis];
        let count = i32(volume.counts[axis]);
        let coord = i32(grid[axis]);

        if (abs(delta) >= count) {
            reset = true;
        } else if (delta > 0) {
            reset = reset || coord >= count - delta;
        } else if (delta < 0) {
            reset = reset || coord < -delta;
        }
    }

    if (reset) {
        probe_states[probe_row].offset = f32x3(0.0);
    }

    let position = volume.probe_position(probe_index) + probe_states[probe_row].offset;

    // Probes behind the camera still light what it sees, so they are slowed down rather than stopped.
    var interval = max_interval;
    if (test_frustum(camera.projection_view, position, volume.spacing * 0.5)) {
        let view_distance = length(position - camera.position.xyz);
        interval = clamp(u32(view_distance / max(full_rate_distance, 1e-3)), 1u, max_interval);
    }

    // Staggering by probe index spreads a given interval's probes evenly over that many frames, so the
    // per frame ray count stays level instead of every slow probe landing on the same frame.
    let update = reset || ((frame_index + probe_index) % interval) == 0;

    // A probe starting over carries no classification either, since the one it had was earned elsewhere.
    probe_states[probe_row].flags = probe_pack_flags(!reset && probe_is_inactive(flags), update, reset, interval);
}
