import common;
import fsr3;

// The SDK gets this out of the luma pyramid's SPD mip 0 store. That pyramid's only other product is
// FrameInfo, which feeds an auto exposure oxylus does not use (eye adaptation already supplies
// one) and a SceneAverageLuma() that no pass ever calls. So the whole pyramid reduces to this.
struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32> farthest_depth;

    RWTexture2D<f32> farthest_depth_mip1;
};
ParameterBlock<ShaderParameters> params;

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let dst_pos = i32x2(thread_id.xy);
    let dst_size = params.constants.render_size / 2;

    if (!fsr3_is_on_screen(dst_pos, dst_size)) {
        return;
    }

    let src_pos = dst_pos * 2;

    var sum = 0.0;
    [ForceUnroll]
    for (i32 y = 0; y < 2; y++) {
        [ForceUnroll]
        for (i32 x = 0; x < 2; x++) {
            let coord = fsr3_clamp_load(src_pos, i32x2(x, y), params.constants.render_size);
            sum += params.farthest_depth.Load(i32x3(coord, 0));
        }
    }

    params.farthest_depth_mip1[thread_id.xy] = sum * 0.25;
}
