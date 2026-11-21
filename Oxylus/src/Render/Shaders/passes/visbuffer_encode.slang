import common;
import gpu;
import scene;
import visbuffer;
import meshlet;

#include <defines.slang>

[[vk::binding(0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(2)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(4)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(5)]] StructuredBuffer<Material> materials;
[[vk::binding(6)]] RWTexture2D<u32> overdraw;
[[vk::constant_id(0)]] const bool RENDER_OVERDRAW = false;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
    nointerpolation u32 vis_data;
    nointerpolation u32 material_index;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID) -> VertexOutput {
    let vis = VisBufferData(vertex_index);
    let source = resolve_meshlet_source(meshes, mesh_instances, transforms, meshlet_instances[vis.meshlet_instance_index]);

    let index = source.meshlet.index(source.mesh_lod, vis.triangle_index);
    let vertex_pos = source.meshlet.position(source.mesh, index);
    let tex_coord = source.meshlet.tex_coord(source.mesh, index);
    let world_pos = source.transform.to_world_position(vertex_pos);
    let clip_pos = camera.jitter_clip(mul(camera.projection_view, f32x4(world_pos.xyz, 1.0)));

    let encoded_vis = VisBufferData(vis.meshlet_instance_index, vis.triangle_index / 3);

    VertexOutput output;
    output.position = clip_pos;
    output.tex_coord = tex_coord;
    output.vis_data = encoded_vis.encode();
    output.material_index = source.mesh_instance.material_index;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> u32 {
#if 1
    let material = materials[NonUniformResourceIndex(input.material_index)];
    if (material.is_alpha_tested()) {
        var alpha = material.get_albedo_color().a;
        if (HAS_FLAG(material.flags, MaterialFlag::HasAlbedoImage)) {
            UVGradient grad;
            grad.uv = input.tex_coord;
            grad.ddx = ddx(input.tex_coord);
            grad.ddy = ddy(input.tex_coord);
            grad.apply_mip_bias(camera.texture_mip_bias);
            alpha = material.sample_albedo_color(grad).a;
        }

        if (alpha < clamp(material.get_alpha_cutoff(), 0.001, 1.0)) {
            discard;
        }
    }
#endif

    if (RENDER_OVERDRAW) {
        __atomic_add(overdraw[u32x2(input.position.xy)], 1u, MemoryOrder::Relaxed);
    }

    return input.vis_data;
}
