import common;
import fsr3;

// Average-of-4 reduction, run once per remaining pyramid mip. This is SpdReduce4; the SDK chains it
// inside one SPD dispatch, we chain it across dispatches instead.
struct ShaderParameters {
    Texture2D<f32x2> src;
    RWTexture2D<f32x2> dst;
};
ParameterBlock<ShaderParameters> params;

struct PushConstants {
    i32x2 dst_size;
    i32x2 src_size;
};

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, uniform PushConstants pc) -> void {
    let dst_pos = i32x2(thread_id.xy);
    if (!fsr3_is_on_screen(dst_pos, pc.dst_size)) {
        return;
    }

    let src_pos = dst_pos * 2;

    var sum = f32x2(0.0);
    [ForceUnroll]
    for (i32 y = 0; y < 2; y++) {
        [ForceUnroll]
        for (i32 x = 0; x < 2; x++) {
            sum += params.src.Load(i32x3(fsr3_clamp_load(src_pos, i32x2(x, y), pc.src_size), 0));
        }
    }

    params.dst[thread_id.xy] = sum * 0.25;
}
