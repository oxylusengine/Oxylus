import common;
import gpu;
import hit;
import refractor;
import scene;
import passes.sssr.sssr_lib;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<u32> refractor_visbuffer;
[[vk::binding(2, 0)]] Texture2D<f32> refractor_depth;
[[vk::binding(3, 0)]] Texture2D<f32> opaque_depth;
[[vk::binding(4, 0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(5, 0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(6, 0)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(7, 0)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(8, 0)]] StructuredBuffer<Material> materials;
[[vk::binding(9, 0)]] Texture2D<f32> depth_hierarchy;
[[vk::binding(10, 0)]] Texture2D<f32x4> opaque_normal_image;
[[vk::binding(11, 0)]] StorageImage2D<u32x4, ImageFormat::RGBA16UI> refractor_reflection;

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, uniform PBRLighting *lighting, uniform SSSRSettings settings) -> void {
    let screen_size = f32x2(settings.render_size);
    if (any(thread_id.xy >= settings.render_size)) {
        return;
    }

    let pixel = i32x2(thread_id.xy);
    let texel = refractor_visbuffer.Load(i32x3(pixel, 0));
    if (texel == ~0u) {
        return;
    }

    let uv = (f32x2(thread_id.xy) + 0.5) / screen_size;
    let geometry = SceneGeometry(mesh_instances, meshes, transforms, materials);
    let refractor = decode_refractor(geometry, meshlet_instances, camera, opaque_depth, lighting.time, texel, uv);
    if (!refractor.is_valid) {
        return;
    }

    let alpha = sssr_roughness_to_alpha(clamp(refractor.surface.roughness, 0.045, 1.0));
    if (!sssr_is_glossy_reflection(alpha, settings.roughness_threshold)) {
        return;
    }

    let z = refractor_depth.Load(i32x3(pixel, 0));
    let view_space_ray = sssr_screen_to_view(camera, uv, z);
    let view_space_normal = mul(camera.view, f32x4(refractor.surface.normal, 0.0)).xyz;
    let view_space_reflected_direction = reflect(normalize(view_space_ray), view_space_normal);

    // projecting the unjittered view position keeps the jitter out of the difference
    let unjittered_origin = sssr_project_position(view_space_ray, camera.projection);
    let screen_space_ray_direction = sssr_project_direction(
        view_space_ray,
        view_space_reflected_direction,
        unjittered_origin,
        camera.projection
    );

    bool valid_hit = false;
    f32 last_surface_z = 1.0;
    let hit = sssr_hierarchical_raymarch(
        depth_hierarchy,
        f32x3(uv, z),
        screen_space_ray_direction,
        true,
        screen_size,
        0,
        settings.min_traversal_occupancy,
        settings.max_traversal_intersections,
        valid_hit,
        last_surface_z
    );
    if (!valid_hit) {
        return;
    }

    let view_space_hit = sssr_screen_to_view(camera, hit.xy, hit.z);
    let world_space_ray = mul(camera.inv_view, f32x4(view_space_hit - view_space_ray, 0.0)).xyz;
    var confidence = sssr_validate_hit(
        depth_hierarchy,
        opaque_normal_image,
        camera,
        hit,
        uv,
        world_space_ray,
        screen_size,
        settings.depth_thickness
    );
    // fades out toward the threshold so a surface crossing it doesn't pop
    confidence *= smoothstep(settings.roughness_threshold, 0.6 * settings.roughness_threshold, alpha);
    if (confidence <= 0.0) {
        return;
    }

    let hit_texel = min(i32x2(screen_size * hit.xy), i32x2(screen_size) - 1);
    refractor_reflection[pixel] = u32x4(u32(hit_texel.x), u32(hit_texel.y), u32(saturate(confidence) * 65535.0 + 0.5), 0u);
}
