import gpu;
import common;
import pbr;
import scene;
import raymarch;
import lighting;

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::constant_id(1)]] const u32 has_directional_shadows = 0;

[[vk::binding(0)]] Sampler linear_clamp_sampler;
[[vk::binding(1)]] Sampler linear_repeat_sampler;
[[vk::binding(2)]] Sampler nearest_clamp_sampler;
[[vk::binding(3)]] Texture2D<f32> depth_image;
[[vk::binding(4)]] Texture2D<f32x4> albedo_image;
[[vk::binding(5)]] Texture2D<f32x4> normal_image;
[[vk::binding(6)]] Texture2D<f32x3> emissive_image;
[[vk::binding(7)]] Texture2D<f32x2> metallic_roughness_image;
[[vk::binding(8)]] Texture2D<f32> gtao_image;
[[vk::binding(9)]] ConstantBuffer<Camera> camera;
[[vk::binding(10)]] Texture2DArray<u32> vsm_page_table;
[[vk::binding(11)]] Texture2DArray<u32> vsm_pointspot_page_tables;
[[vk::binding(12)]] Texture2D<f32> vsm_physical_pages;

func make_lighting_resources(
    PBRLighting *lighting,
    Light *lights,
    LightGridCell *light_grid,
    VSMPointSpotView *vsm_pointspot_views,
    VirtualClipmap *vsm_clipmaps
) -> LightingResources {
    return LightingResources(
        vsm_page_table,
        vsm_pointspot_page_tables,
        vsm_physical_pages,
        nearest_clamp_sampler,
        camera,
        lighting,
        lights,
        light_grid,
        vsm_pointspot_views,
        vsm_clipmaps
    );
}

func contact_shadow_visibility(f32x2 uv, in PBRLighting lighting) -> f32 {
    u32 width, height, levels;
    depth_image.GetDimensions(0, width, height, levels);
    let image_size = u32x2(width, height);

    f32 shadow = 0;
    f32 raymarch_jitter = 1.0;

    f32 depth_sample = depth_image.SampleLevel(nearest_clamp_sampler, uv, 0);

    const f32x3 cs = f32x3(uv * 2.0 - 1.0, depth_sample);
    const f32x3 ws = camera.unproject_uv(uv, depth_sample);

    f32 rcp_near_plane_distance = 1.0f / camera.near_clip;

    f32x3 shadow_direction = normalize(lighting.sun_direction);
    f32x3 shadow_end = ws + shadow_direction * lighting.contact_shadow_length;
    // march in the jittered grid that the depth texture was rasterized into
    var jittered_projection = camera.projection;
    jittered_projection[0] += (2.0 * camera.temporalaa_jitter.x / camera.resolution.x) * camera.projection[3];
    jittered_projection[1] += (2.0 * camera.temporalaa_jitter.y / camera.resolution.y) * camera.projection[3];

    DepthRayMarchResult raymarch_result = DepthRayMarch
      ::new_from_depth(image_size)
      .from_cs(cs)
      .to_ws(shadow_end, camera.view, jittered_projection)
      .with_linear_steps(lighting.contact_shadow_steps)
      .with_depth_thickness(lighting.contact_shadow_thickness)
      .with_jitter(raymarch_jitter)
      .with_march_behind_surfaces(false)
      .march(depth_image, linear_clamp_sampler, nearest_clamp_sampler, rcp_near_plane_distance);

    if (raymarch_result.hit) {
        shadow = smoothstep(1.0, 0.3, raymarch_result.hit_penetration_frac);
    }

    return 1.0 - shadow;
}

func sun_visibility(
    in LightingResources resources,
    f32x2 uv,
    f32x2 frag_coord,
    f32 depth,
    f32x3 smooth_normal,
    f32x3 world_position,
    bool has_directional_light
) -> f32 {
    let directional = lighting_directional_visibility(
        resources, scene_flags, has_directional_shadows, uv, frag_coord, depth, smooth_normal, world_position, has_directional_light);

    var contact = 1.0;
    if ((scene_flags & SceneFlags::HasContactShadows) != 0) {
        contact = contact_shadow_visibility(uv, *resources.lighting);
    }

    return directional * contact;
}
