import common;
import gpu;
import scene;

[[vk::binding(0)]] RaytracingAccelerationStructure scene_tlas;
[[vk::binding(1)]] ConstantBuffer<Camera> camera;
[[vk::binding(2)]] Image2D<f32> depth_image;
[[vk::binding(3)]] Image2D<f32x4> normal_image;
[[vk::binding(4)]] RWTexture2D<f32, 1, ImageFormat.R16F> ambient_occlusion;
[[vk::binding(5)]] Sampler nearest_clamp_sampler;

func pcg_hash(u32 v) -> u32 {
    let state = v * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

func next_f32(inout u32 seed) -> f32 {
    seed = pcg_hash(seed);
    return f32(seed) * 2.3283064365386963e-10;
}

func cosine_hemisphere(f32x2 u) -> f32x3 {
    let phi = TAU * u.x;
    let sin_theta = sqrt(u.y);
    return f32x3(cos(phi) * sin_theta, sin(phi) * sin_theta, sqrt(1.0 - u.y));
}

func trace_occluded(f32x3 origin, f32x3 direction, f32 t_min, f32 t_max) -> bool {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = t_min;
    ray.TMax = t_max;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene_tlas, RAY_FLAG_NONE, 0xffu, ray);
    query.Proceed();

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[[shader("compute")]]
[[require(spvRayQueryKHR)]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x3 thread_id : SV_DispatchThreadID,
    uniform u32 ray_count,
    uniform f32 radius,
    uniform f32 power,
    uniform u32 frame_index
) -> void {
    u32 width, height, level_count;
    depth_image.GetDimensions(0, width, height, level_count);
    let image_size = u32x2(width, height);
    let coords = thread_id.xy;
    if (any(coords >= image_size)) {
        return;
    }

    let uv = (f32x2(coords) + 0.5) / f32x2(image_size);
    let depth = depth_image.SampleLevel(nearest_clamp_sampler, uv, 0.0);
    if (depth == 0.0) {
        ambient_occlusion[coords] = 1.0;
        return;
    }

    let world_position = camera.unproject_uv(uv, depth);
    let packed_normal = normal_image.SampleLevel(nearest_clamp_sampler, uv, 0.0);
    let world_normal = normalize(com::oct_to_vec3(packed_normal.zw));
    let basis = com::build_orthonormal_basis(world_normal);

    let view_distance = length(world_position - camera.position.xyz);
    let origin_bias = 1e-3 + view_distance * 1e-3;

    var seed = pcg_hash(coords.x + pcg_hash(coords.y + pcg_hash(frame_index)));
    var occlusion = 0.0;

    let ray_count = max(ray_count, 1u);
    for (u32 i = 0; i < ray_count; i++) {
        let jitter = f32x2(next_f32(seed), next_f32(seed));
        let sample = com::hammersley2d(i, ray_count);
        let direction = mul(basis, cosine_hemisphere(frac(sample + jitter)));

        if (trace_occluded(world_position + world_normal * origin_bias, direction, origin_bias, radius)) {
            occlusion += 1.0;
        }
    }

    ambient_occlusion[coords] = pow(saturate(1.0 - occlusion / f32(ray_count)), power);
}
