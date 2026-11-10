import common;
import gpu;
import scene;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Texture2D<f32x4> radiance_image;

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    u32 width;
    u32 height;
    radiance_image.GetDimensions(width, height);
    let pixel = min(u32x2(input.tex_coord * f32x2(f32(width), f32(height))), u32x2(width - 1u, height - 1u));

    let radiance = radiance_image.Load(u32x3(pixel, 0u)).rgb / RTGI_RADIANCE_SCALE;
    return f32x4(radiance / (1.0 + radiance), 1.0);
}
