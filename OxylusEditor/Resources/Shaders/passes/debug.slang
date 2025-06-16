module debug;

import gpu;
import scene;
import passes.visbuffer;

#include <fullscreen.slang>

[[vk::binding(0, 0)]]
Sampler linear_repeat_sampler;

[[vk::binding(1, 0)]]
Image2D<u32> visbuffer_data;

[[vk::binding(2, 0)]]
Image2D<f32> depth_image;

[[vk::binding(3, 0)]]
Image2D<u32> overdraw;

[[vk::binding(4, 0)]]
Image2D<f32x4> albedo_image;

[[vk::binding(5, 0)]]
Image2D<f32x4> normal_image;

[[vk::binding(6, 0)]]
Image2D<f32x3> emissive_image;

[[vk::binding(7, 0)]]
Image2D<f32x3> metallic_roughness_occlusion_image;

[[vk::binding(8, 0)]]
ConstantBuffer<Camera, ScalarDataLayout> camera;

[[vk::binding(9, 0)]]
StructuredBuffer<u32, ScalarDataLayout> visible_meshlet_instances_indices;

struct PushConstants {
    DebugView debug_view;
    f32 heatmap_scale;
};
[[vk::push_constant]] PushConstants C;

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
func fs_main(VertexOutput input) -> f32x4 {
    const u32 texel = visbuffer_data.load(u32x2(input.position.xy));
    if (texel == ~0u) {
        discard;
    }

    f32x3 color = f32x3(0.0, 0.0, 0.0);
    const let vis_data = VisBufferData(texel);
    switch (C.debug_view) {
        case DebugView::Triangles: {
            const u32 h = hash(vis_data.triangle_index);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::Meshlets: {
            const u32 h = hash(visible_meshlet_instances_indices[vis_data.meshlet_instance_index]);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::Overdraw: {
            const f32 draw_scale = clamp(C.heatmap_scale, 0.0, 100.0) / 100.0;
            const f32 draw_count = f32(overdraw.load(u32x2(input.position.xy)));
            const f32 heat = 1.0 - exp2(-draw_count * draw_scale);
            color = inferno(heat);
        } break;
        case DebugView::Albedo: {
            color = albedo_image.sample_mip(linear_repeat_sampler, input.tex_coord, 0).rgb;
        } break;
        case DebugView::Normal: {
            const f32x4 sampled_normal = normal_image.load(u32x2(input.position.xy));
            f32x3 mapped_normal = com::oct_to_vec3(sampled_normal.xy);
            color = mapped_normal * 0.5 + 0.5;
        } break;
        case DebugView::Emissive: {
            color = emissive_image.load(u32x2(input.position.xy)).rgb;
        } break;
        case DebugView::Metallic: {
            color = metallic_roughness_occlusion_image.load(u32x2(input.position.xy)).rrr;
        } break;
        case DebugView::Roughness: {
            color = metallic_roughness_occlusion_image.load(u32x2(input.position.xy)).ggg;
        } break;
        case DebugView::Occlusion: {
            color = metallic_roughness_occlusion_image.load(u32x2(input.position.xy)).bbb;
        } break;
        default:;
    }

    return f32x4(color, 1.0);
}
