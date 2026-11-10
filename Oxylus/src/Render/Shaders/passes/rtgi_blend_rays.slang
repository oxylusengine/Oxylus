// adapted from Timberdoodle (Apache-2.0)
import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(2, 0)]] Texture2D<u32> ray_count_image;
[[vk::binding(3, 0)]] Texture2D<u32> pixel_ray_alloc_image;
[[vk::binding(4, 0)]] StructuredBuffer<RTGIRayResult> ray_result;
[[vk::binding(5, 0)]] ConstantBuffer<HistogramLuminance> exposure_buffer;
[[vk::binding(6, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> perceptual_rgb_shortness_image;
[[vk::binding(7, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> radiance_image;

[[shader("compute")]]
[numthreads(16, 16, 1)]
func cs_main(u32x2 half_pixel : SV_DispatchThreadID, uniform RTGISettings settings) {
    if (any(half_pixel >= settings.half_size)) {
        return;
    }

    let load = u32x3(half_pixel, 0u);
    let depth = half_res_depth_image.Load(load);
    let ray_count = ray_count_image.Load(load);
    if (depth == 0.0 || ray_count == 0u) {
        perceptual_rgb_shortness_image[half_pixel] = f32x4(0.0);
        radiance_image[half_pixel] = f32x4(0.0);
        return;
    }

    let uv = (f32x2(half_pixel) + 0.5) / f32x2(settings.half_size);
    let pixel_width_ws = rtgi_pixel_width_ws(camera, uv, depth);
    let exposure = rtgi_current_exposure(settings, exposure_buffer.exposure);

    let first_ray = pixel_ray_alloc_image.Load(load);
    let inv_count = 1.0 / f32(ray_count);
    var mean_perceptual_rgb = f32x3(0.0);
    var mean_shortness = 0.0;
    var mean_radiance = f32x3(0.0);
    for (u32 i = 0u; i < ray_count; i++) {
        let result = ray_result[first_ray + i];
        mean_perceptual_rgb += rtgi_linear_to_perceptual_rgb(result.radiance, exposure) * inv_count;
        mean_shortness += rtgi_calc_ray_shortness(result.t, pixel_width_ws, settings.max_visibility_pixel_range) * inv_count;
        mean_radiance += result.radiance * inv_count;
    }

    perceptual_rgb_shortness_image[half_pixel] = f32x4(mean_perceptual_rgb, mean_shortness);

    // the stored value is scaled, half precision tops out at com::HALF_MAX
    radiance_image[half_pixel] = f32x4(min(mean_radiance, com::HALF_MAX), 1.0);
}
