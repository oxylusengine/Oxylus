import common;
import gpu;
import scene;
import cull;
import meshlet;
import rmvsm;

#include <defines.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(3)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(4)]] StructuredBuffer<Material> materials;
[[vk::binding(5)]] StructuredBuffer<VirtualClipmap> clipmaps;
[[vk::binding(6)]] Texture2DArray<u32> page_tables;
[[vk::binding(7)]] RWTexture2D<u32, 1, ImageFormat::R32UI> physical_pages;
[[vk::binding(8)]] StructuredBuffer<MeshletInstanceVisibility> visibility;
[[vk::binding(9)]] Texture2DArray<u8> hpb;
[[vk::binding(10)]] SamplerState hpb_sampler;
[[vk::binding(11)]] StructuredBuffer<u32> draw_clipmaps;

[[vk::push_constant]] VSMContext ctx;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
    nointerpolation u32 material_index : MATERIAL;
    nointerpolation u32 clipmap_index : CLIPMAP;
};

struct PrimitiveOutput {
    bool culled : SV_CullPrimitive;
};

groupshared u32 visible_meshlet_count;
struct VSMTaskPayload {
    u32 base_meshlet_instance_index;
    u32 clipmap_index;
    u8 visible_indices[CULLING_MESHLET_COUNT];
};

groupshared VSMTaskPayload payload;

[[shader("task")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func ts_main(
    u32 meshlet_instance_index : SV_DispatchThreadID,
    u32 local_index : SV_GroupIndex,
    u32 draw_id : SV_DrawIndex
) -> void {
    if (local_index == 0) {
        visible_meshlet_count = 0;
        payload.base_meshlet_instance_index = meshlet_instance_index;
        payload.clipmap_index = draw_clipmaps[draw_id];
    }

    GroupMemoryBarrierWithGroupSync();

    if (meshlet_instance_index < visibility[0].total_visible_meshlet_instances) {
        let clipmap_index = payload.clipmap_index;
        let clipmap = clipmaps[clipmap_index];
        let meshlet_instance = meshlet_instances[meshlet_instance_index];
        let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
        let transform = transforms[mesh_instance.transform_index];
        let mvp = mul(clipmap.projection_view_mat, transform.world);

        let mesh = meshes[mesh_instance.mesh_index];
        let mesh_lod = mesh.lods[mesh_instance.lod_index];
        let meshlet_bounds = mesh_lod.meshlet_bounds[meshlet_instance.meshlet_index];
        let aabb_center = meshlet_bounds.get_aabb_center();
        let aabb_extent = meshlet_bounds.get_aabb_extent();
        let cone_cutoff = meshlet_bounds.get_cone_cutoff();

        let cone_axis = normalize(transform.to_world_normal(meshlet_bounds.get_cone_axis()));
        let view_dir = -ctx.directional_light_dir;
        let cone_visible = cone_cutoff >= 1.0 || !test_cone_directional(cone_axis, cone_cutoff, view_dir);

        if (cone_visible && test_frustum(mvp, aabb_center, aabb_extent)) {
            var visible = true;
            if (let screen_aabb = project_aabb(mvp, clipmap.z_near, aabb_center, aabb_extent)) {
                visible = test_vsm_page(screen_aabb, hpb, hpb_sampler, clipmap_index, clipmap.page_offset);
            }

            if (visible) {
                let slot = __atomic_add(visible_meshlet_count, 1, MemoryOrder::Relaxed);
                payload.visible_indices[slot] = u8(local_index);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(visible_meshlet_count, 1, 1, payload);
}

groupshared f32x4 clip_positions_shared[MESHLET_MAX_VERTICES];

[[shader("mesh")]]
[[outputtopology("triangle")]]
[[numthreads(64, 1, 1)]]
func ms_main(
    u32x3 group_id : SV_GroupID,
    u32 local_index : SV_GroupIndex,
    in payload VSMTaskPayload payload,
    OutputIndices<u32x3, MESHLET_MAX_PRIMITIVES> triangles,
    OutputVertices<VertexOutput, MESHLET_MAX_VERTICES> vertices,
    OutputPrimitives<PrimitiveOutput, MESHLET_MAX_PRIMITIVES> primitives
) -> void {
    let slot = group_id.x;
    let meshlet_instance_index = payload.base_meshlet_instance_index + u32(payload.visible_indices[slot]);
    let meshlet_instance = meshlet_instances[meshlet_instance_index];
    let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let meshlet = mesh_lod.meshlets[meshlet_instance.meshlet_index];
    let vertex_count = u32(meshlet.vertex_count);
    let triangle_count = u32(meshlet.triangle_count);
    SetMeshOutputCounts(vertex_count, triangle_count);

    let mvp = mul(clipmaps[payload.clipmap_index].projection_view_mat, transforms[mesh_instance.transform_index].world);

    if (local_index < vertex_count) {
        let vertex_index = mesh_lod.indirect_vertex_indices[meshlet.indirect_vertex_index_offset + local_index];
        let clip_pos = mul(mvp, f32x4(meshlet.position(mesh, vertex_index), 1.0));
        clip_positions_shared[local_index] = clip_pos;

        VertexOutput output;
        output.position = clip_pos;
        output.tex_coord = meshlet.tex_coord(mesh, vertex_index);
        output.material_index = mesh_instance.material_index;
        output.clipmap_index = payload.clipmap_index;
        vertices[local_index] = output;
    }

    GroupMemoryBarrierWithGroupSync();

    if (local_index < triangle_count) {
        let base_triangle_index = meshlet.local_triangle_index_offset + local_index * 3;
        let corners = u32x3(
            Meshlet.get_micro_index(mesh_lod.local_triangle_indices, base_triangle_index + 0),
            Meshlet.get_micro_index(mesh_lod.local_triangle_indices, base_triangle_index + 1),
            Meshlet.get_micro_index(mesh_lod.local_triangle_indices, base_triangle_index + 2)
        );
        triangles[local_index] = corners;
        let clip_pos = f32x4x3(
            clip_positions_shared[corners.x],
            clip_positions_shared[corners.y],
            clip_positions_shared[corners.z]
        );

        PrimitiveOutput output;
        let in_front = clip_pos[0].z >= 0.0 && clip_pos[1].z >= 0.0 && clip_pos[2].z >= 0.0;
        output.culled = !in_front || test_triangle_backface(clip_pos);
        primitives[local_index] = output;
    }
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> void {
    let clipmap_index = input.clipmap_index;
    let clipmap = clipmaps[clipmap_index];
    let page_offset = clipmap.page_offset;

    let virtual_uv = input.position.xy / ctx.virtual_extent;
    let virt_coords = i32x2(floor(virtual_uv * ctx.get_page_table_extent().xy));
    let wrapped_coords = ctx.page_coords_virtual_to_wrapped(virt_coords, page_offset);
    let page_address = i32x3(wrapped_coords, clipmap_index);
    let page = VSMPageMetadata(page_tables.Load(i32x4(page_address, 0)));
    if (page.is_backed() && page.is_dirty()) {
        let physical_page_coords = page.get_physical_coords(ctx.physcial_page_table_size, ctx.page_size);
        let virtual_texel_coord = i32x2(floor(virtual_uv * ctx.virtual_extent));
        let in_page_texel_coord = i32x2(com::mod(f32x2(virtual_texel_coord), f32x2(ctx.page_size)));
        let in_memory_offset = physical_page_coords * ctx.page_size;
        let physical_texel_coord = in_memory_offset + in_page_texel_coord;

        let material = materials[NonUniformResourceIndex(input.material_index)];
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

        __atomic_min(physical_pages[physical_texel_coord], asuint(input.position.z / input.position.w));
    }
}
