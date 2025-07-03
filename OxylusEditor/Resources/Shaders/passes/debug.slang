module debug;

import gpu;
import scene;
import visbuffer;
import cull;

#include <fullscreen.slang>

struct ShaderParameters {
    Sampler linear_repeat_sampler;
    Image2D<u32> visbuffer_data;
    Image2D<f32> depth_image;
    Image2D<u32> overdraw;
    Image2D<f32x4> albedo_image;
    Image2D<f32x4> normal_image;
    Image2D<f32x3> emissive_image;
    Image2D<f32x3> metallic_roughness_occlusion_image;
    Image2D<f32> hiz_image;

    ConstantBuffer<Camera> camera;
    StructuredBuffer<u32> visible_meshlet_instances_indices;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;
};

func hash(uint a) -> u32 {
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}

func inferno(f32 t) -> f32x3 {
    let c0 = f32x3(0.0002189403691192265, 0.001651004631001012, -0.01948089843709184);
    let c1 = f32x3(0.1065134194856116, 0.5639564367884091, 3.932712388889277);
    let c2 = f32x3(11.60249308247187, -3.972853965665698, -15.9423941062914);
    let c3 = f32x3(-41.70399613139459, 17.43639888205313, 44.35414519872813);
    let c4 = f32x3(77.162935699427, -33.40235894210092, -81.80730925738993);
    let c5 = f32x3(-71.31942824499214, 32.62606426397723, 73.20951985803202);
    let c6 = f32x3(25.13112622477341, -12.24266895238567, -23.07032500287172);

    t = saturate(t);
    return c0 + t * (c1 + t * (c2 + t * (c3 + t * (c4 + t * (c5 + t * c6)))));
}

[[shader("fragment")]]
func fs_main(
    VertexOutput input,
    uniform ShaderParameters params,
    uniform DebugView debug_view,
    uniform f32 heatmap_scale
) -> f32x4 {
    let texel = params.visbuffer_data.load(u32x2(input.position.xy));
    if (texel == ~0u) {
        discard;
    }

    var color = f32x3(0.0, 0.0, 0.0);
    let vis_data = VisBufferData(texel);
    switch (debug_view) {
        case DebugView::Triangles: {
            let h = hash(vis_data.triangle_index);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::Meshlets: {
            let h = hash(params.visible_meshlet_instances_indices[vis_data.meshlet_instance_index]);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::Overdraw: {
            let draw_scale = clamp(heatmap_scale, 0.0, 100.0) / 100.0;
            let draw_count = f32(params.overdraw.load(u32x2(input.position.xy)));
            let heat = 1.0 - exp2(-draw_count * draw_scale);
            color = inferno(heat);
        } break;
        case DebugView::Albedo: {
            color = params.albedo_image.sample_mip(params.linear_repeat_sampler, input.tex_coord, 0).rgb;
        } break;
        case DebugView::Normal: {
            let sampled_normal = params.normal_image.load(u32x2(input.position.xy));
            f32x3 mapped_normal = com::oct_to_vec3(sampled_normal.xy);
            color = mapped_normal * 0.5 + 0.5;
        } break;
        case DebugView::Emissive: {
            color = params.emissive_image.load(u32x2(input.position.xy)).rgb;
        } break;
        case DebugView::Metallic: {
            color = params.metallic_roughness_occlusion_image.load(u32x2(input.position.xy)).rrr;
        } break;
        case DebugView::Roughness: {
            color = params.metallic_roughness_occlusion_image.load(u32x2(input.position.xy)).ggg;
        } break;
        case DebugView::Occlusion: {
            color = params.metallic_roughness_occlusion_image.load(u32x2(input.position.xy)).bbb;
        } break;
        case DebugView::HiZ: {
            let meshlet_instance_index = params.visible_meshlet_instances_indices[vis_data.meshlet_instance_index];
            let meshlet_instance = params.meshlet_instances[meshlet_instance_index];
            let mesh = params.meshes[meshlet_instance.mesh_index];
            let meshlet_index = meshlet_instance.meshlet_index;
            let meshlet = mesh.meshlets[meshlet_index];
            let transform = params.transforms[meshlet_instance.transform_index];
            let bounds = mesh.meshlet_bounds[meshlet_index];
            let aabb_min = transform.to_world_position(bounds.aabb_min).xyz;
            let aabb_max = transform.to_world_position(bounds.aabb_max).xyz;
            let aabb_extent = aabb_max - aabb_min;

            let mvp = mul(params.camera.projection_view, transform.world);
            var max_depth = 0.0;
            if (let ret = project_aabb(mvp, params.camera.near_clip, aabb_min, aabb_extent)) {
                ScreenAabb screen_aabb = ret;
                let hiz_size = ceil(params.camera.resolution * 0.5);
                let aabb_min = screen_aabb.min.xy * hiz_size;
                let aabb_max = screen_aabb.max.xy * hiz_size;
            
                let min_texel = u32x2(max(aabb_min, f32x2(0.0)));
                let max_texel = u32x2(min(aabb_max, hiz_size - 1.0));
                let size = max_texel - min_texel;
                let max_size = max(size.x, size.y);
                var mip = max(firstbithigh(max_size) + 1, 2) - 2;
        
                if (any((max_texel >> u32x2(mip)) > (min_texel >> u32x2(mip)) + 3)) {
                    mip += 1;
                }
        
                let smin = min_texel >> u32x2(mip);
                let smax = max_texel >> u32x2(mip);

                color = f32x3(smin, mip);
            }
        } break;
        default:;
    }

    return f32x4(color, 1.0);
}
