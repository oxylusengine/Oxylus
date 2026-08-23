import common;
import gpu;
import scene;
import visbuffer;
import rmvsm;

#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(3)]] StructuredBuffer<Material> materials;
[[vk::binding(4)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(5)]] StructuredBuffer<VSMMeshletInstance> vsm_meshlet_instances;
[[vk::binding(6)]] Texture2DArray<u32> page_tables;
[[vk::binding(7)]] RWTexture2D<u32, 1, ImageFormat::R32UI> physical_pages;

[[vk::push_constant]] VSMPointSpotContext ctx;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
    nointerpolation u32 material_index;
    nointerpolation u32 layer;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID) -> VertexOutput {
    let vis = VisBufferData(vertex_index);
    let vsm_meshlet_instance = vsm_meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = mesh_instances[vsm_meshlet_instance.mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let transform = transforms[mesh_instance.transform_index];
    let meshlet = mesh_lod.meshlets[vsm_meshlet_instance.meshlet_index];

    let index = meshlet.index(mesh_lod, vis.triangle_index);
    let vertex_pos = meshlet.position(mesh, index);
    let tex_coord = meshlet.tex_coord(mesh, index);
    let world_pos = transform.to_world_position(vertex_pos);

    let projection_view = views[vsm_meshlet_instance.layer].projection_view;
    let clip_pos = mul(projection_view, f32x4(world_pos.xyz, 1.0));

    VertexOutput output;
    output.position = clip_pos;
    output.tex_coord = tex_coord;
    output.material_index = mesh_instance.material_index;
    output.layer = vsm_meshlet_instance.layer;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> void {
    let virtual_texel_coord = i32x2(input.position.xy);
    let page_coords = virtual_texel_coord / VSM_PAGE_SIZE;
    let page_address = i32x3(page_coords, input.layer);
    let page = VSMPageMetadata(page_tables.Load(i32x4(page_address, ctx.curr_mip)));
    if (page.is_backed() && page.is_dirty()) {
        let physical_page_coords = page.get_physical_coords(VSM_PHYSICAL_PAGE_TABLE_SIZE, VSM_PAGE_SIZE);
        let in_page_texel_coord = virtual_texel_coord % VSM_PAGE_SIZE;
        let in_memory_offset = physical_page_coords * VSM_PAGE_SIZE;
        let physical_texel_coord = in_memory_offset + in_page_texel_coord;

        let material = materials[input.material_index];
        if (material.flags & MaterialFlag::HasAlbedoImage) {
            UVGradient grad;
            grad.uv = input.tex_coord;
            grad.ddx = ddx(input.tex_coord);
            grad.ddy = ddy(input.tex_coord);
            let alpha_color = material.sample_albedo_color_grad(grad).a;

            if (alpha_color < clamp(material.get_alpha_cutoff(), 0.001, 1.0)) {
                return;
            }
        }

        __atomic_min(physical_pages[physical_texel_coord], asuint(saturate(input.position.z)));
    }
}
