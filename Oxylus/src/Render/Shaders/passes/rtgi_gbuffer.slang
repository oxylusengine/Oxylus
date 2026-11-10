import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> albedo_image;
[[vk::binding(3, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(4, 0)]] Texture2D<f32x2> metallic_roughness_image;
[[vk::binding(5, 0)]] StorageImage2D<f32, ImageFormat::R32F> half_res_depth_image;
[[vk::binding(6, 0)]] StorageImage2D<u32, ImageFormat::R32UI> half_res_face_normal_image;
[[vk::binding(7, 0)]] StorageImage2D<u32, ImageFormat::R32UI> half_res_normal_roughness_image;
[[vk::binding(8, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA8> half_res_albedo_metalness_image;

[[shader("compute")]]
[numthreads(16, 16, 1)]
func cs_main(u32x2 half_pixel : SV_DispatchThreadID, uniform u32x2 render_size) {
    let half_size = (render_size + 1u) / 2u;
    if (any(half_pixel >= half_size)) {
        return;
    }

    // a quad on an odd edge has pixels outside the render area, those count as sky
    var linear_depths = f32x4(0.0);
    for (u32 i = 0u; i < 4u; i++) {
        let pixel = half_pixel * 2u + u32x2(i & 1u, i >> 1u);
        if (all(pixel < render_size)) {
            let depth = depth_image.Load(u32x3(pixel, 0u));
            linear_depths[i] = select(depth == 0.0, 0.0, camera.linear_depth(depth));
        }
    }

    let best = rtgi_quad_downsample_representative(linear_depths);
    let best_pixel = min(half_pixel * 2u + u32x2(u32(best) & 1u, u32(best) >> 1u), render_size - 1u);
    let best_load = u32x3(best_pixel, 0u);

    let is_sky = linear_depths[best] == 0.0;
    var face_normal = f32x3(0.0, 0.0, 1.0);
    var shading_normal = f32x3(0.0, 0.0, 1.0);
    var roughness = 1.0;
    var albedo_metalness = f32x4(0.0);
    if (!is_sky) {
        let normals = normal_image.Load(best_load);
        shading_normal = com::oct_to_vec3(normals.rg);
        face_normal = com::oct_to_vec3(normals.ba);

        let metallic_roughness = metallic_roughness_image.Load(best_load);
        roughness = metallic_roughness.g;
        albedo_metalness = f32x4(saturate(albedo_image.Load(best_load).rgb), saturate(metallic_roughness.r));
    }

    half_res_depth_image[half_pixel] = linear_depths[best];
    half_res_face_normal_image[half_pixel] = select(is_sky, 0u, com::compress_normal_32(face_normal));
    half_res_normal_roughness_image[half_pixel] = select(is_sky, 0u, rtgi_pack_normal_roughness(shading_normal, roughness));
    half_res_albedo_metalness_image[half_pixel] = albedo_metalness;
}
