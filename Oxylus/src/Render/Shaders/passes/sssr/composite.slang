import common;
import gpu;
import pbr;
import scene;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> albedo_image;
[[vk::binding(3, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(4, 0)]] Texture2D<f32x2> metallic_roughness_image;
[[vk::binding(5, 0)]] Texture2D<f32x4> reflection_image;

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    let pixel_pos = u32x3(u32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);
    if (depth == 0.0) {
        return f32x4(0.0);
    }

    let albedo_color = albedo_image.Load(pixel_pos).rgb;
    let N = normalize(com::oct_to_vec3(normal_image.Load(pixel_pos).xy));

    let metallic_roughness = metallic_roughness_image.Load(pixel_pos);
    let metallic = clamp(metallic_roughness.r, 0.0, 1.0);
    let roughness = clamp(metallic_roughness.g, 0.045, 1.0);

    let world_position = camera.unproject_uv(input.tex_coord, depth);
    let V = normalize(camera.position.xyz - world_position);

    let NoV = abs(dot(N, V)) + 1e-5;
    let F0 = lerp(f32x3(0.04), albedo_color, metallic);
    let AB = GGX_directional_albedo(NoV, perceptual_roughness_to_alpha(roughness));
    let kS = F0 * AB.x + AB.y;

    let radiance = reflection_image.Load(pixel_pos).rgb;
    return f32x4(kS * radiance, 1.0);
}
