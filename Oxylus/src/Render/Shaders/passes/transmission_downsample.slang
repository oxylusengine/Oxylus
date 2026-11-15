import common;
import gpu;

[[vk::constant_id(0)]] const bool IS_COPY = false;

[[vk::binding(0, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> dst_image;
[[vk::binding(1, 0)]] Image2D<f32x4> src_image;
[[vk::binding(2, 0)]] Sampler linear_clamp_sampler;

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x2 coords : SV_DispatchThreadID, uniform u32x2 dst_size) -> void {
    if (any(coords >= dst_size)) {
        return;
    }

    var src_width = 0u;
    var src_height = 0u;
    src_image.GetDimensions(src_width, src_height);

    if (IS_COPY) {
        dst_image.Store(coords, src_image.Load(i32x3(coords, 0)));
        return;
    }

    // a [1 3 3 1] binomial kernel from four bilinear taps, a box chain blends between mips as a visible double image
    let texel = 1.0 / f32x2(src_width, src_height);
    let center = (f32x2(coords) * 2.0 + 1.0) * texel;
    let offset = 0.75 * texel;
    let a = src_image.SampleLevel(linear_clamp_sampler, center + f32x2(-offset.x, -offset.y), 0.0);
    let b = src_image.SampleLevel(linear_clamp_sampler, center + f32x2(offset.x, -offset.y), 0.0);
    let c = src_image.SampleLevel(linear_clamp_sampler, center + f32x2(-offset.x, offset.y), 0.0);
    let d = src_image.SampleLevel(linear_clamp_sampler, center + f32x2(offset.x, offset.y), 0.0);

    dst_image.Store(coords, (a + b + c + d) * 0.25);
}
