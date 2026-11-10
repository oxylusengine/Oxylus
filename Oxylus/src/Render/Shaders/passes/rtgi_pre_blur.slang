import common;
import gpu;
import scene;
import embedded.possion;
import rtgi_common;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> half_res_depth_image;
[[vk::binding(2, 0)]] Texture2D<u32> half_res_face_normal_image;
[[vk::binding(3, 0)]] Texture2D<f32x4> diffuse_before_image;
[[vk::binding(4, 0)]] Texture2D<f32x2> diffuse2_before_image;
[[vk::binding(5, 0)]] Texture2D<f32> firefly_factor_image;
[[vk::binding(6, 0)]] Texture2D<f32> perceptual_radiance_image;
[[vk::binding(7, 0)]] Texture2D<f32> ao_guide_image;
[[vk::binding(8, 0)]] Texture2D<u32> ray_count_image;
[[vk::binding(9, 0)]] StorageImage2D<f32x4, ImageFormat::RGBA16F> diffuse_blurred_image;
[[vk::binding(10, 0)]] StorageImage2D<f32x2, ImageFormat::RG16F> diffuse2_blurred_image;

static const u32 SAMPLE_COUNT = 8u;
static const f32 AO_GUIDE_FLOOR = 0.2;
static const f32 PERCEPTUAL_RADIANCE_GUIDE_TOLERANCE = 0.8;
static const f32 RESTIR_IMPORTANCE_FLOOR = 1.0e-6;
static const f32 SS_GRADIENT_VIEW_BIAS = 0.01;
static const f32 GOLDEN_ANGLE = 2.39996323;

[[shader("compute")]]
[numthreads(16, 16, 1)]
func cs_main(u32x2 half_pixel : SV_DispatchThreadID, uniform RTGISettings settings, uniform u32 iteration) {
    if (any(half_pixel >= settings.half_size)) {
        return;
    }

    let load = i32x3(i32x2(half_pixel), 0);
    let depth = half_res_depth_image.Load(load);
    if (depth == 0.0) {
        diffuse_blurred_image[half_pixel] = f32x4(0.0);
        diffuse2_blurred_image[half_pixel] = f32x2(0.0);
        return;
    }

    let half_size = f32x2(settings.half_size);
    let uv = (f32x2(half_pixel) + 0.5) / half_size;
    let position_ws = camera.world_position_from_linear_depth(uv, depth);
    let normal_ws = com::decompress_normal_32(half_res_face_normal_image.Load(load));
    let normal_vs = mul(camera.view, f32x4(normal_ws, 0.0)).xyz;
    let rcp_pixel_width_ws = 1.0 / max(rtgi_pixel_width_ws(camera, uv, depth), 1.0e-8);

    let iter_shift = 1743216u;
    let prime_shift0 = 257u;
    let prime_shift1 = 9629u;
    let prime_shift2 = 10069u;
    com::rand_seed(iter_shift * iteration + settings.frame_index * prime_shift0 + half_pixel.x * prime_shift1 + half_pixel.y * prime_shift2);

    let pixel_perceptual_radiance = perceptual_radiance_image.Load(load);
    if (isinf(pixel_perceptual_radiance)) {
        diffuse_blurred_image[half_pixel] = diffuse_before_image.Load(load);
        diffuse2_blurred_image[half_pixel] = diffuse2_before_image.Load(load);
        return;
    }

    let ao_guide = ao_guide_image.Load(load);
    let ao_guide_radius_scale = lerp(AO_GUIDE_FLOOR, 1.0, ao_guide);
    let iteration_scaling = rsqrt(f32(max(settings.pre_blur_iterations, 1u)));
    let blur_radius = max(1.0, settings.pre_blur_base_width * ao_guide_radius_scale * iteration_scaling);

    // the kernel follows the surface, the normal is nudged to the camera so grazing angles keep some footprint
    let biased_vs_normal = lerp(normal_vs, f32x3(0.0, 0.0, 1.0), SS_GRADIENT_VIEW_BIAS);
    let ss_gradient = f32x2(
        sin(acos(clamp(biased_vs_normal.x, -1.0, 1.0))),
        sin(acos(clamp(biased_vs_normal.y, -1.0, 1.0)))
    );

    let sample_count = max(1u, u32(f32(SAMPLE_COUNT) / f32(max(settings.pre_blur_iterations, 1u))));

    let rotation_angle = (f32(settings.frame_index) + f32(iteration) * 0.61803399) * GOLDEN_ANGLE;
    let rotation_sin = sin(rotation_angle);
    let rotation_cos = cos(rotation_angle);

    // clamped fireflies are spread to more pixels, which hands back the energy the clamp removed
    let is_firefly_compensating = iteration == 0u;
    let center_ray_count_weight = max(1.0, f32(ray_count_image.Load(load)));

    // weighted reservoir over the pixel and its taps, importance is the clamped brightness
    var held_sh = diffuse_before_image.Load(load);
    var held_index = i32x2(half_pixel);
    var held_importance = max(held_sh.w, RESTIR_IMPORTANCE_FLOOR);
    let center_factor = select(is_firefly_compensating, firefly_factor_image.Load(load), 1.0);
    var weight_sum = center_ray_count_weight * center_factor * held_importance;
    var mean_weight_sum = center_ray_count_weight * center_factor;

    let poisson_offset = settings.frame_index & 7u;
    for (u32 s = 0u; s < sample_count - 1u; s++) {
        let poisson = POSSION_16[(s + poisson_offset) & 15u].xy;
        let disc_noise = f32x2(
            rotation_cos * poisson.x - rotation_sin * poisson.y,
            rotation_sin * poisson.x + rotation_cos * poisson.y
        );
        let sample_2d = disc_noise * blur_radius;
        let sample_uv = uv + ss_gradient * sample_2d / half_size;

        // the wiggle moves taps off the centre texel and keeps the kernel from collapsing at shallow angles
        let pixel_index_wiggle = disc_noise * 0.5;
        let sample_index_unclamped = i32x2(sample_uv * half_size + pixel_index_wiggle);
        let is_out_of_bounds = or(any(sample_index_unclamped < 0), any(sample_index_unclamped >= i32x2(settings.half_size)));
        let sample_index = clamp(sample_index_unclamped, i32x2(0), i32x2(settings.half_size) - 1);
        let sample_load = i32x3(sample_index, 0);

        let sample_depth = half_res_depth_image.Load(sample_load);
        let is_sky = sample_depth == 0.0;
        let sample_uv_center = (f32x2(sample_index) + 0.5) / half_size;
        let sample_position_ws = camera.world_position_from_linear_depth(sample_uv_center, sample_depth);
        let sample_normal_ws = com::decompress_normal_32(half_res_face_normal_image.Load(sample_load));
        let sample_sh_y = diffuse_before_image.Load(sample_load);
        let sample_perceptual_radiance = perceptual_radiance_image.Load(sample_load);

        let geometric_weight = rtgi_calc_similar_surface_weight(
            rcp_pixel_width_ws, position_ws, normal_ws, sample_position_ws, sample_normal_ws
        );
        let normal_weight = rtgi_calc_similar_normal_weight(normal_ws, sample_normal_ws);

        let perceptual_difference_weight = select(
            isinf(sample_perceptual_radiance),
            1.0,
            rtgi_calc_perceptual_difference_weight(
                pixel_perceptual_radiance, sample_perceptual_radiance, PERCEPTUAL_RADIANCE_GUIDE_TOLERANCE
            )
        );

        let firefly_power = select(is_firefly_compensating, firefly_factor_image.Load(sample_load), 1.0);
        let sample_ray_count_weight = max(1.0, f32(ray_count_image.Load(sample_load)));

        let is_valid = and(!is_sky, !is_out_of_bounds);
        let confidence = select(
            is_valid,
            geometric_weight * normal_weight * perceptual_difference_weight * sample_ray_count_weight,
            0.0
        );

        // the selects keep an infinite importance on a tap without confidence from turning the sums into nan
        let is_confident = confidence > 0.0;
        let sample_importance = max(sample_sh_y.w, RESTIR_IMPORTANCE_FLOOR);
        let mean_weight = select(is_confident, confidence * firefly_power, 0.0);
        let selection_weight = select(is_confident, mean_weight * sample_importance, 0.0);
        weight_sum += selection_weight;
        mean_weight_sum += mean_weight;

        let is_selected = and(is_confident, com::rand() * weight_sum < selection_weight);
        held_sh = select(is_selected, sample_sh_y, held_sh);
        held_index = select(is_selected, sample_index, held_index);
        held_importance = select(is_selected, sample_importance, held_importance);
    }

    // contribution weight of the kept tap, the output brightness is the firefly weighted neighbourhood mean
    let reservoir_mean = weight_sum / max(mean_weight_sum, 1.0e-8);
    let contribution_weight = reservoir_mean / max(held_importance, RESTIR_IMPORTANCE_FLOOR);
    diffuse_blurred_image[half_pixel] = held_sh * contribution_weight;
    diffuse2_blurred_image[half_pixel] = diffuse2_before_image.Load(i32x3(held_index, 0)) * contribution_weight;
}
