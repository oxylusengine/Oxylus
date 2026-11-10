import common;
import gpu;
import scene;
import rtgi_common;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Texture2D<f32x4> diffuse_image;
[[vk::binding(1, 0)]] Texture2D<f32x2> diffuse2_image;
[[vk::binding(2, 0)]] Texture2D<u32> normal_roughness_image;

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    u32 width;
    u32 height;
    diffuse_image.GetDimensions(width, height);
    let pixel = min(u32x2(input.tex_coord * f32x2(f32(width), f32(height))), u32x2(width - 1u, height - 1u));
    let load = u32x3(pixel, 0u);

    let normal = rtgi_unpack_normal(normal_roughness_image.Load(load));
    let radiance = rtgi_sh_resolve_diffuse(diffuse_image.Load(load), diffuse2_image.Load(load), normal) / RTGI_RADIANCE_SCALE;
    return f32x4(radiance / (1.0 + radiance), 1.0);
}
