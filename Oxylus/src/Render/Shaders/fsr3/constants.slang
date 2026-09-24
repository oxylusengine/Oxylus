implementing fsr3;

// mirrors GPU::FSR3Constants in Scene/SceneGPU.hpp. the SDK packs these as loose arrays in
// cbFSR3Upscaler; the layout here is the same order so the accessors below stay recognisable
// against the reference
public struct FSR3Constants {
    public i32x2 render_size;
    public i32x2 previous_frame_render_size;

    public i32x2 upscale_size;
    public i32x2 previous_frame_upscale_size;

    public i32x2 max_render_size;
    public i32x2 max_upscale_size;

    public f32x4 device_to_view_depth;

    public f32x2 jitter_offset;
    public f32x2 previous_frame_jitter_offset;

    public f32x2 motion_vector_scale;
    public f32x2 downscale_factor;

    public f32x2 motion_vector_jitter_cancellation;
    public f32 tan_half_fov;
    public f32 jitter_phase_count;

    public f32 delta_time;
    public f32 delta_pre_exposure;
    public f32 view_space_to_meters_factor;
    public f32 frame_index;

    public f32 velocity_factor;
    public f32 reactiveness_scale;
    public f32 shading_change_scale;
    public f32 accumulation_added_per_frame;
    public f32 min_disocclusion_accumulation;
};
