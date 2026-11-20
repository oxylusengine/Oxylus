import gpu;
import common;
import pbr;
import scene;
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

[[vk::binding(17)]] Texture2D<f32> contact_visibility;

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
        depth_image,
        (scene_flags & SceneFlags::HasLocalContactShadows) != 0,
        true,
        camera,
        lighting,
        lights,
        light_grid,
        vsm_pointspot_views,
        vsm_clipmaps
    );
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
        contact = lerp(1.0, contact_visibility.Load(int3(int2(frag_coord), 0)), resources.lighting.sun_contact_shadow_strength);
    }

    return min(directional, contact);
}
