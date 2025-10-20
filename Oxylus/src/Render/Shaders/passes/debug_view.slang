import gpu;
import scene;
import visbuffer;
import cull;

#include <fullscreen.slang>

struct ShaderParameters {
    Sampler linear_repeat_sampler;
    Image2D<u32> visbuffer_data;
    Image2D<f32> depth;
    Image2D<u32> overdraw;
    Image2D<f32x4> albedo;
    Image2D<f32x4> normal;
    Image2D<f32x3> emissive;
    Image2D<f32x3> metallic_roughness_occlusion;
    Image2D<f32> ambient_occlusion;

    StructuredBuffer<u32> visible_meshlet_instances_indices;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<MeshInstance> mesh_instances;
    StructuredBuffer<Mesh> meshes;
};
ParameterBlock<ShaderParameters> params;

func hsv_to_rgb(f32x3 hsv) -> f32x3 {
    let n = f32x3(5.0, 3.0, 1.0);
    let k = (n + hsv.x / 1.0471975512) % 6.0;
    return hsv.z - hsv.z * hsv.y * max(f32x3(0.0), min(k, min(4.0 - k, f32x3(1.0))));
}

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

func sobel_outline(u32x2 pixel_pos, f32x3 color) -> f32x3 {
    static int sobel_x[] = {
        1, 0, -1,
        2, 0, -2,
        1, 0, -1
    };
    static int sobel_y[] = {
         1,  2,  1,
         0,  0,  0,
        -1, -2, -1
    };
    static u32x2 sample_offset[] = {
        {-1,  1},  {0,  1},  {1,  1},
        {-1,  0},  {0,  0},  {1,  0},
        {-1, -1},  {0, -1},  {1, -1}
    };

    var x = f32x3(0.0);
    var y = f32x3(0.0);
    for (uint tap = 0; tap < 9; tap++) {
        let sample_pos = i32x2(pixel_pos) + sample_offset[tap];
        let texel = params.visbuffer_data.load(sample_pos);
        if (texel == ~0u) {
            continue;
        }

        let depth_raw = params.depth.load(sample_pos);
        let sample_density_depth = log2(depth_raw + 1.0) * 10.0;

        x += f32(sobel_x[tap]) * sample_density_depth;
        y += f32(sobel_y[tap]) * sample_density_depth;
    }

    let depth_outline = max(abs(x), abs(y));
    return saturate(color * (1.0 - depth_outline));
}

[[shader("fragment")]]
func fs_main(
    VertexOutput input,
    uniform DebugView debug_view,
    uniform f32 heatmap_scale
) -> f32x4 {
    let pixel_pos = u32x2(input.position.xy);
    let texel = params.visbuffer_data.load(pixel_pos);
    if (texel == ~0u) {
        discard;
    }

    var color = f32x3(0.0, 0.0, 0.0);
    let vis = VisBufferData(texel);
    let meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];
    let mesh = params.meshes[mesh_instance.mesh_index];

    switch (debug_view) {
        case DebugView::Triangles: {
            let h = hash(vis.triangle_index);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::Meshlets: {
            let h = hash(vis.meshlet_instance_index);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::Overdraw: {
            let draw_scale = clamp(heatmap_scale, 0.0, 100.0) / 100.0;
            let draw_count = f32(params.overdraw.load(pixel_pos));
            let heat = 1.0 - exp2(-draw_count * draw_scale);
            color = inferno(heat);
        } break;
        case DebugView::Materials: {
            let h = hash(mesh_instance.material_index);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::MeshInstances: {
            let h = hash(mesh_instance.mesh_index);
            color = f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0;
        } break;
        case DebugView::MeshLods: {
            let lod_color = f32x3(f32(mesh_instance.lod_index) / f32(mesh.lod_count + 1) * TAU, 1.0, 0.5);
            color = hsv_to_rgb(lod_color);
        } break;
        case DebugView::Albedo: {
            color = params.albedo.sample_mip(params.linear_repeat_sampler, input.tex_coord, 0).rgb;
        } break;
        case DebugView::Normal: {
            let sampled_normal = params.normal.load(pixel_pos);
            f32x3 mapped_normal = com::oct_to_vec3(sampled_normal.xy);
            color = mapped_normal * 0.5 + 0.5;
        } break;
        case DebugView::Emissive: {
            color = params.emissive.load(pixel_pos).rgb;
        } break;
        case DebugView::Metallic: {
            color = params.metallic_roughness_occlusion.load(pixel_pos).rrr;
        } break;
        case DebugView::Roughness: {
            color = params.metallic_roughness_occlusion.load(pixel_pos).ggg;
        } break;
        case DebugView::BakedOcclusion: {
            color = params.metallic_roughness_occlusion.load(pixel_pos).bbb;
        } break;
        case DebugView::GTAO: {
            color = params.ambient_occlusion.load(pixel_pos).rrr;
        } break;
        default:;
    }

    color = sobel_outline(pixel_pos, color);

    return f32x4(color, 1.0);
}
