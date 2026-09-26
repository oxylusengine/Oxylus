import common;
import fsr3;

// Produces mip 0 of the shading change pyramid at render/2. The SDK folds this into an SPD dispatch
// that also emits mips 1..n; here mip 0 is written by this pass and the rest by fsr3_downsample,
// which is the same average-of-4 reduction SpdReduce4 performs.
//
// .x carries the mean signed luma difference and .y the mean of its sign, so the shading change
// pass can take |x*y|: a region whose differences agree in sign keeps |y| near 1 and survives,
// while noise that cancels drives y toward 0 and is rejected.
struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32> current_luma;
    Texture2D<f32> previous_luma;
    Texture2D<f32x2> dilated_motion_vectors;
    ConstantBuffer<FSR3Exposure> exposure;

    RWTexture2D<f32x2> spd_mip0;
};
ParameterBlock<ShaderParameters> params;

static const i32x2 SAMPLE_OFFSETS[SHADING_CHANGE_SET_SIZE] = {
    i32x2(+0, +0), i32x2(-1, +0), i32x2(+1, +0), i32x2(+0, -1), i32x2(+0, +1)
};

struct SampleSet {
    f32 samples[SHADING_CHANGE_SET_SIZE];
};

// fixed 9-comparator sorting network for 5 elements
func sort_set(inout SampleSet set) -> void {
    const i32x2 pairs[9] = {
        i32x2(0, 3), i32x2(1, 4), i32x2(0, 2), i32x2(1, 3), i32x2(0, 1),
        i32x2(2, 4), i32x2(1, 2), i32x2(3, 4), i32x2(2, 3),
    };

    [ForceUnroll]
    for (i32 k = 0; k < 9; k++) {
        let i = pairs[k].x;
        let j = pairs[k].y;
        let tmp = min(set.samples[i], set.samples[j]);
        set.samples[j] = max(set.samples[i], set.samples[j]);
        set.samples[i] = tmp;
    }
}

// merge-walks the two sorted sets looking for the smallest relative difference: the pair of samples
// most likely to be the same surface across the two frames
func compute_minimum_difference(SampleSet set0_in, SampleSet set1_in) -> f32 {
    var set0 = set0_in;
    var set1 = set1_in;

    var min_diff = FSR3_FP16_MAX - 1.0;
    var a = 0;
    var b = 0;

    sort_set(set0);
    sort_set(set1);

    let max_value = min(set0.samples[SHADING_CHANGE_SET_SIZE - 1], set1.samples[SHADING_CHANGE_SET_SIZE - 1]);

    if (max_value > FSR3_FP32_MIN) {
        for (i32 i = 0; i < SHADING_CHANGE_SET_SIZE && (min_diff < FSR3_FP16_MAX); i++) {
            var diff = set0.samples[a] - set1.samples[b];

            if (abs(diff) > FSR3_FP16_MIN) {
                diff = sign(diff) * (1.0 - fsr3_min_divided_by_max(set0.samples[a], set1.samples[b]));

                min_diff = (abs(diff) < abs(min_diff)) ? diff : min_diff;

                a += i32(set0.samples[a] < set1.samples[b]);
                b += i32(set0.samples[a] >= set1.samples[b]);
            } else {
                min_diff = FSR3_FP16_MAX;
            }
        }
    }

    return min_diff * f32(min_diff < (FSR3_FP16_MAX - 1.0));
}

// the SDK raises each sample to fShadingChangeSamplePow, which is 1.0/1.0, so the pow is identity
// and is dropped here
func get_current_luma_samples(f32x2 uv, f32 exposure) -> SampleSet {
    let uv_jittered = uv + params.constants.jitter_offset / f32x2(params.constants.render_size);
    let base_pos = i32x2(floor(uv_jittered * f32x2(params.constants.render_size)));

    SampleSet set;
    [ForceUnroll]
    for (i32 i = 0; i < SHADING_CHANGE_SET_SIZE; i++) {
        let pos = fsr3_clamp_load(base_pos, SAMPLE_OFFSETS[i], params.constants.render_size);
        set.samples[i] = max(params.current_luma.Load(i32x3(pos, 0)) * exposure, FSR3_EPSILON);
    }

    return set;
}

struct PreviousLumaSamples {
    SampleSet set;
    bool is_existing_sample;
};

func get_previous_luma_samples(f32x2 uv, f32x2 motion_vector, f32 exposure) -> PreviousLumaSamples {
    PreviousLumaSamples data;
    data.set = {};

    let uv_jittered = uv + params.constants.previous_frame_jitter_offset /
                           f32x2(params.constants.previous_frame_render_size);
    let reprojected_uv = uv_jittered + motion_vector;

    data.is_existing_sample = fsr3_is_uv_inside(reprojected_uv);

    if (data.is_existing_sample) {
        let base_pos = i32x2(floor(reprojected_uv * f32x2(params.constants.previous_frame_render_size)));

        [ForceUnroll]
        for (i32 i = 0; i < SHADING_CHANGE_SET_SIZE; i++) {
            let pos = fsr3_clamp_load(base_pos, SAMPLE_OFFSETS[i], params.constants.previous_frame_render_size);
            let luma = params.previous_luma.Load(i32x3(pos, 0)) * params.constants.delta_pre_exposure * exposure;
            data.set.samples[i] = max(luma, FSR3_EPSILON);
        }
    }

    return data;
}

func compute_diff(f32x2 uv, f32x2 motion_vector, f32 exposure) -> f32 {
    let current_samples = get_current_luma_samples(uv, exposure);
    let previous = get_previous_luma_samples(uv, motion_vector, exposure);

    if (!previous.is_existing_sample) {
        return 0.0;
    }

    return compute_minimum_difference(current_samples, previous.set);
}

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let dst_pos = i32x2(thread_id.xy);
    let dst_size = params.constants.render_size / 2;

    if (!fsr3_is_on_screen(dst_pos, dst_size)) {
        return;
    }

    let exposure = params.exposure.value();
    let src_pos = dst_pos * 2;

    // average the 2x2 render resolution block, matching SPD's first reduction
    var sum = f32x2(0.0);
    [ForceUnroll]
    for (i32 y = 0; y < 2; y++) {
        [ForceUnroll]
        for (i32 x = 0; x < 2; x++) {
            let coord = fsr3_clamp_load(src_pos, i32x2(x, y), params.constants.render_size);
            let motion_vector = params.dilated_motion_vectors.Load(i32x3(coord, 0));
            let uv = (f32x2(coord) + 0.5) / f32x2(params.constants.render_size);

            let diff = compute_diff(uv, motion_vector, exposure);
            sum += f32x2(diff, (diff != 0.0) ? sign(diff) : 0.0);
        }
    }

    params.spd_mip0[thread_id.xy] = sum * 0.25;
}
