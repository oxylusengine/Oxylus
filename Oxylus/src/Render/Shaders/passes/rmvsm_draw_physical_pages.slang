import common;
import gpu;
import scene;
import visbuffer;
import rmvsm;

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(3)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(4)]] StructuredBuffer<Material> materials;
[[vk::binding(5)]] StructuredBuffer<VirtualClipmap> clipmaps;
[[vk::binding(6)]] Texture2DArray<u32> page_tables;
[[vk::binding(7)]] RWTexture2D<u32, 1, ImageFormat::R32UI> physical_pages;

[[vk::push_constant]] VSMContext ctx;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
    nointerpolation u32 material_index;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID) -> VertexOutput {
    let vis = VisBufferData(vertex_index);
    let meshlet_instance = meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let transform = transforms[mesh_instance.transform_index];
    let meshlet = mesh_lod.meshlets[meshlet_instance.meshlet_index];

    let index = meshlet.index(mesh_lod, vis.triangle_index);
    let vertex_pos = meshlet.position(mesh, index);
    let tex_coord = meshlet.tex_coord(mesh, index);
    let world_pos = transform.to_world_position(vertex_pos);

    let projection_view_mat = clipmaps[ctx.curr_clipmap_index].projection_view_mat;
    let clip_pos = mul(projection_view_mat, f32x4(world_pos.xyz, 1.0));

    VertexOutput output;
    output.position = clip_pos;
    output.tex_coord = tex_coord;
    output.material_index = mesh_instance.material_index;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> void {
    let clipmap = clipmaps[ctx.curr_clipmap_index];
    let page_offset = clipmap.page_offset;

    let virtual_uv = input.position.xy / ctx.get_physical_page_table_extent().xy;
    let virt_coords = i32x2(floor(virtual_uv * ctx.get_page_table_extent().xy));
    let wrapped_coords = ctx.page_coords_virtual_to_wrapped(virt_coords, page_offset);
    let page_address = i32x3(wrapped_coords, ctx.curr_clipmap_index);
    let page = VSMPageMetadata(page_tables.Load(i32x4(page_address, 0)));
    if (page.is_backed() && page.is_dirty()) {
        let physical_page_coords = page.get_physical_coords(ctx.physcial_page_table_size, ctx.page_size);
        let virtual_texel_coord = i32x2(floor(virtual_uv * ctx.physcial_page_table_size));
        let in_page_texel_coord = i32x2(com::mod(f32x2(virtual_texel_coord), f32x2(ctx.page_size)));
        let in_memory_offset = physical_page_coords * ctx.page_size;
        let physical_texel_coord = in_memory_offset + in_page_texel_coord;

#if 1
        let material = materials[input.material_index];
        if (material.flags & MaterialFlag::HasAlbedoImage) {
            UVGradient grad;
            grad.uv = input.tex_coord;
            grad.ddx = ddx(input.tex_coord);
            grad.ddy = ddy(input.tex_coord);
            let alpha_color = material.sample_albedo_color(grad).a;

            if (alpha_color < clamp(material.get_alpha_cutoff(), 0.001, 1.0)) {
                return;
            }
        }
#endif
        __atomic_min(physical_pages[physical_texel_coord], asuint(input.position.z / input.position.w));
    }
}
