import common;
import gpu;
import scene;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Texture2D<f32> firefly_factor_image;

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    u32 width;
    u32 height;
    firefly_factor_image.GetDimensions(width, height);
    let pixel = min(u32x2(input.tex_coord * f32x2(f32(width), f32(height))), u32x2(width - 1u, height - 1u));

    // the factor is energy before over energy after the clamp, 1 when nothing was clamped
    let factor = max(firefly_factor_image.Load(u32x3(pixel, 0u)), 1.0);
    return f32x4(com::inferno(1.0 - 1.0 / factor), 1.0);
}
