module debug;

import gpu;
import scene;
import visbuffer;
import cull;

#include <fullscreen.slang>

struct ShaderParameters {
    Sampler linear_repeat_sampler;
    Sampler hiz_sampler;
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
    return f32x4(color, 1.0);
}
