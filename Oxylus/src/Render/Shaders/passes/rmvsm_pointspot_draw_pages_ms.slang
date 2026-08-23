import common;
import gpu;
import scene;
import cull;
import meshlet;
import rmvsm;

#include <defines.slang>
#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(3)]] StructuredBuffer<Material> materials;
[[vk::binding(4)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(5)]] StructuredBuffer<VSMMeshletInstance> vsm_meshlet_instances;
[[vk::binding(6)]] StructuredBuffer<u32> vsm_meshlet_instance_count;
[[vk::binding(7)]] Texture2DArray<u8> hpb;
[[vk::binding(8)]] Texture2DArray<u32> page_tables;
[[vk::binding(9)]] RWTexture2D<u32, 1, ImageFormat::R32UI> physical_pages;

[[vk::push_constant]] VSMPointSpotContext ctx;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord;
    nointerpolation u32 material_index;
    nointerpolation u32 layer;
};

struct PrimitiveOutput {
    bool culled : SV_CullPrimitive;
};

groupshared u32 visible_meshlet_count;
groupshared MeshletTaskPayload payload;

[[shader("task")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func ts_main(
    u32 vsm_meshlet_instance_index : SV_DispatchThreadID,
    u32 local_index : SV_GroupIndex
) -> void {
    if (local_index == 0) {
        visible_meshlet_count = 0;
        payload.base_meshlet_instance_index = vsm_meshlet_instance_index;
    }

    GroupMemoryBarrierWithGroupSync();

    let instance_count = min(vsm_meshlet_instance_count[0], VSM_POINT_SPOT_MAX_MESHLET_INSTANCES);
    if (vsm_meshlet_instance_index < instance_count) {
        let vsm_meshlet_instance = vsm_meshlet_instances[vsm_meshlet_instance_index];
        let mesh_instance = mesh_instances[vsm_meshlet_instance.mesh_instance_index];
        let transform = transforms[mesh_instance.transform_index];
        let view = views[vsm_meshlet_instance.layer];
        let mvp = mul(view.projection_view, transform.world);

        let mesh = meshes[mesh_instance.mesh_index];
        let mesh_lod = mesh.lods[mesh_instance.lod_index];
        let meshlet_bounds = mesh_lod.meshlet_bounds[vsm_meshlet_instance.meshlet_index];
        let aabb_center = meshlet_bounds.get_aabb_center();
        let aabb_extent = meshlet_bounds.get_aabb_extent();
        let cone_cutoff = meshlet_bounds.get_cone_cutoff();

        let cone_axis = normalize(transform.to_world_normal(meshlet_bounds.get_cone_axis()));
        let world_center = transform.to_world_position(aabb_center).xyz;
        let world_radius = transform.to_world_radius(length(aabb_extent * 0.5));
        let in_range = distance(world_center, view.light_position) - world_radius <= view.range;
        let cone_visible = cone_cutoff >= 1.0 ||
                           !test_cone(world_center, world_radius, cone_axis, cone_cutoff, view.light_position);

        if (in_range && cone_visible && test_frustum(mvp, aabb_center, aabb_extent)) {
            var visible = true;
            if (let screen_aabb = project_aabb(mvp, view.z_near, aabb_center, aabb_extent)) {
                visible = test_pointspot_pages(screen_aabb, hpb, vsm_meshlet_instance.layer, ctx.curr_mip);
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
    in payload MeshletTaskPayload payload,
    OutputIndices<u32x3, MESHLET_MAX_PRIMITIVES> triangles,
    OutputVertices<VertexOutput, MESHLET_MAX_VERTICES> vertices,
    OutputPrimitives<PrimitiveOutput, MESHLET_MAX_PRIMITIVES> primitives
) -> void {
    let slot = group_id.x;
    let vsm_meshlet_instance_index = payload.base_meshlet_instance_index + u32(payload.visible_indices[slot]);
    let vsm_meshlet_instance = vsm_meshlet_instances[vsm_meshlet_instance_index];
    let mesh_instance = mesh_instances[vsm_meshlet_instance.mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let meshlet = mesh_lod.meshlets[vsm_meshlet_instance.meshlet_index];
    let vertex_count = u32(meshlet.vertex_count);
    let triangle_count = u32(meshlet.triangle_count);
    SetMeshOutputCounts(vertex_count, triangle_count);

    let transform = transforms[mesh_instance.transform_index];
    let mvp = mul(views[vsm_meshlet_instance.layer].projection_view, transform.world);

    if (local_index < vertex_count) {
        let vertex_index = mesh_lod.indirect_vertex_indices[meshlet.indirect_vertex_index_offset + local_index];
        let clip_pos = mul(mvp, f32x4(meshlet.position(mesh, vertex_index), 1.0));
        clip_positions_shared[local_index] = clip_pos;

        VertexOutput output;
        output.position = clip_pos;
        output.tex_coord = meshlet.tex_coord(mesh, vertex_index);
        output.material_index = mesh_instance.material_index;
        output.layer = vsm_meshlet_instance.layer;
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

        let any_in_front = clip_pos[0].w > 0.0 || clip_pos[1].w > 0.0 || clip_pos[2].w > 0.0;
        let all_in_front = clip_pos[0].w > 0.0 && clip_pos[1].w > 0.0 && clip_pos[2].w > 0.0;

        PrimitiveOutput output;
        output.culled = !any_in_front || (all_in_front && test_triangle_backface(clip_pos));
        primitives[local_index] = output;
    }
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

        let material = materials[NonUniformResourceIndex(input.material_index)];
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
