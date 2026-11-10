import common;
import gpu;
import rtgi_common;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(1, 0)]] Texture2D<u32> half_res_normal_roughness_image;

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    let is_normal_half = input.tex_coord.x < 0.5;
    let half_uv = f32x2(input.tex_coord.x * 2.0 - select(is_normal_half, 0.0, 1.0), input.tex_coord.y);

    u32 width;
    u32 height;
    half_res_depth_image.GetDimensions(width, height);
    let pixel = min(u32x2(half_uv * f32x2(f32(width), f32(height))), u32x2(width - 1u, height - 1u));

    let depth = half_res_depth_image.Load(u32x3(pixel, 0u));
    if (depth == 0.0) {
        return f32x4(0.0, 0.0, 0.0, 1.0);
    }

    if (is_normal_half) {
        let normal = rtgi_unpack_normal(half_res_normal_roughness_image.Load(u32x3(pixel, 0u)));
        return f32x4(normal * 0.5 + 0.5, 1.0);
    }

    return f32x4((1.0 - exp(-depth / 50.0)).xxx, 1.0);
}
