import common;
import gpu;
import scene;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Texture2D<u32> ray_count_image;

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform RTGISettings settings) -> f32x4 {
    u32 width;
    u32 height;
    ray_count_image.GetDimensions(width, height);
    let pixel = min(u32x2(input.tex_coord * f32x2(f32(width), f32(height))), u32x2(width - 1u, height - 1u));

    let ray_count = ray_count_image.Load(u32x3(pixel, 0u));
    return f32x4(com::inferno(f32(ray_count) / f32(max(settings.max_rays_per_pixel, 1u))), 1.0);
}
