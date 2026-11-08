import shared;

#define COLOR_GRADING_STOPS 16.0
#define COLOR_GRADING_MID_GREY 0.18

f32x3 color_grading_log_encode(f32x3 color) {
    return saturate(log2(max(color, 1e-10) / COLOR_GRADING_MID_GREY) / COLOR_GRADING_STOPS + 0.5);
}

f32x3 color_grading_lut_uvw(f32x3 color) {
    let t = color_grading_log_encode(color);
    return t * (f32(COLOR_GRADING_LUT_SIZE - 1) / f32(COLOR_GRADING_LUT_SIZE)) + 0.5 / f32(COLOR_GRADING_LUT_SIZE);
}

// texel 0 evaluates at exactly zero so black stays black
f32 color_grading_log_decode(u32 index) {
    if (index == 0) {
        return 0.0;
    }

    let t = f32(index) / f32(COLOR_GRADING_LUT_SIZE - 1);
    return COLOR_GRADING_MID_GREY * exp2((t - 0.5) * COLOR_GRADING_STOPS);
}
