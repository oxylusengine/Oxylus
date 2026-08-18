import common;
import gpu;
import scene;
import cull;
import visbuffer;
import meshlet;

#include <defines.slang>

[[vk::constant_id(0)]] const u32 CULL_FLAGS;
[[vk::constant_id(1)]] const bool RENDER_OVERDRAW = false;

[[vk::binding(0)]] Image2D<f32> hiz_image;
[[vk::binding(1)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(2)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(3)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(4)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(5)]] StructuredBuffer<MeshletInstanceVisibility> visibility;
[[vk::binding(6)]] RWStructuredBuffer<u32> meshlet_instance_visibility_mask;
[[vk::binding(7)]] StructuredBuffer<Material> materials;
[[vk::binding(8)]] StorageImage2D<u32, ImageFormat::R32UI> overdraw;

[[vk::push_constant]] CullCamera camera;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
    nointerpolation u32 meshlet_instance_index : MESHLETINSTANCE;
    nointerpolation u32 material_index : MATERIAL;
};

struct PrimitiveOutput {
    u32 triangle_index : SV_PrimitiveID;
    bool culled : SV_CullPrimitive;
};

groupshared u32 visible_meshlet_count;
groupshared MeshletTaskPayload payload;

[[shader("task")]]
[[numthreads(TASK_MESHLET_COUNT, 1, 1)]]
func ts_main(
    u32 meshlet_instance_index : SV_DispatchThreadID,
    u32 local_index : SV_GroupIndex
) -> void {
    if (local_index == 0) {
        visible_meshlet_count = 0;
        payload.base_meshlet_instance_index = meshlet_instance_index;
    }

    GroupMemoryBarrierWithGroupSync();

    if (meshlet_instance_index < visibility[0].total_visible_meshlet_instances) {
        let meshlet_instance = meshlet_instances[meshlet_instance_index];
        let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
        let transform = transforms[mesh_instance.transform_index];
        let mvp = mul(camera.projection_view, transform.world);

        let mesh = meshes[mesh_instance.mesh_index];
        let mesh_lod = mesh.lods[mesh_instance.lod_index];
        let meshlet_bounds = mesh_lod.meshlet_bounds[meshlet_instance.meshlet_index];
        let aabb_center = meshlet_bounds.get_aabb_center();
        let aabb_extent = meshlet_bounds.get_aabb_extent();
        let cone_cutoff = meshlet_bounds.get_cone_cutoff();

        var visibility_mask_index = 0u;
        var visibility_bit = 0u;
        var was_visible = true;
        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestOcclusion)) {
            let meshlet_instance_visibility_index = mesh_instance.meshlet_instance_visibility_offset + meshlet_instance.meshlet_index;
            visibility_mask_index = meshlet_instance_visibility_index / 32;
            let bit_index = meshlet_instance_visibility_index - visibility_mask_index * 32;
            visibility_bit = 1u << bit_index;
            was_visible = (meshlet_instance_visibility_mask[visibility_mask_index] & visibility_bit) != 0;
        }

        let cone_axis = normalize(transform.to_world_normal(meshlet_bounds.get_cone_axis()));
        let world_center = transform.to_world_position(aabb_center).xyz;
        let world_radius = transform.to_world_radius(length(aabb_extent * 0.5));

        var visible = HAS_FLAG(CULL_FLAGS, CullFlag.LatePass) ? true : was_visible;
        visible = visible && (cone_cutoff >= 1.0 || !test_cone(world_center, world_radius, cone_axis, cone_cutoff, camera.position));
        visible = visible && test_frustum(mvp, aabb_center, aabb_extent);

        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestOcclusion | CullFlag.LatePass) && visible) {
            if (let screen_aabb = project_aabb(mvp, camera.near_clip, aabb_center, aabb_extent)) {
                visible = !test_occlusion(screen_aabb, hiz_image);
            }
        }

        if (visible && (!HAS_FLAG(CULL_FLAGS, CullFlag.LatePass) || !was_visible)) {
            let meshlet = mesh_lod.meshlets[meshlet_instance.meshlet_index];
            let slot = __atomic_add(visible_meshlet_count, 1, MemoryOrder::Relaxed);
            payload.visible_indices[slot] = u8(local_index);
            payload.vertex_counts[slot] = u8(meshlet.vertex_count);
            payload.triangle_counts[slot] = u8(meshlet.triangle_count);
        }

        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestOcclusion | CullFlag.LatePass)) {
            if (visible) {
                __atomic_or(meshlet_instance_visibility_mask[visibility_mask_index], visibility_bit, MemoryOrder::Relaxed);
            } else {
                __atomic_and(meshlet_instance_visibility_mask[visibility_mask_index], ~visibility_bit, MemoryOrder::Relaxed);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(visible_meshlet_count, 1, 1, payload);
}

groupshared f32x4 clip_positions_shared[MESHLET_MAX_VERTICES];

[[shader("mesh")]]
[[outputtopology("triangle")]]
[[numthreads(MESH_SHADER_THREAD_COUNT, 1, 1)]]
func ms_main(
    u32x3 group_id : SV_GroupID,
    u32 local_index : SV_GroupIndex,
    in payload MeshletTaskPayload payload,
    OutputIndices<u32x3, MESHLET_MAX_PRIMITIVES> triangles,
    OutputVertices<VertexOutput, MESHLET_MAX_VERTICES> vertices,
    OutputPrimitives<PrimitiveOutput, MESHLET_MAX_PRIMITIVES> primitives
) -> void {
    let slot = group_id.x;
    let vertex_count = u32(payload.vertex_counts[slot]);
    let triangle_count = u32(payload.triangle_counts[slot]);
    SetMeshOutputCounts(vertex_count, triangle_count);

    let meshlet_instance_index = payload.base_meshlet_instance_index + u32(payload.visible_indices[slot]);
    let meshlet_instance = meshlet_instances[meshlet_instance_index];
    let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let meshlet = mesh_lod.meshlets[meshlet_instance.meshlet_index];

    let mvp = mul(camera.projection_view, transforms[mesh_instance.transform_index].world);

    if (local_index < vertex_count) {
        let vertex_index = mesh_lod.indirect_vertex_indices[meshlet.indirect_vertex_index_offset + local_index];
        let clip_pos = mul(mvp, f32x4(meshlet.position(mesh, vertex_index), 1.0));
        clip_positions_shared[local_index] = clip_pos;

        VertexOutput output;
        output.position = clip_pos;
        output.tex_coord = meshlet.tex_coord(mesh, vertex_index);
        output.meshlet_instance_index = meshlet_instance_index;
        output.material_index = mesh_instance.material_index;
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
        output.triangle_index = local_index;
        let in_front = clip_pos[0].z >= 0.0 && clip_pos[1].z >= 0.0 && clip_pos[2].z >= 0.0;
        output.culled = !in_front || test_triangle_backface(clip_pos);
        primitives[local_index] = output;
    }
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> u32 {
    let material = materials[NonUniformResourceIndex(input.material_index)];
    if (HAS_FLAG(material.flags, MaterialFlag::HasAlbedoImage)) {
        UVGradient grad;
        grad.uv = input.tex_coord;
        grad.ddx = ddx(input.tex_coord);
        grad.ddy = ddy(input.tex_coord);
        let alpha_color = material.sample_albedo_color(grad).a;

        // We are doing deferred, blend alpha mode is not supported in this pass.
        if (alpha_color < clamp(material.get_alpha_cutoff(), 0.001, 1.0)) {
            discard;
        }
    }

    if (RENDER_OVERDRAW) {
        __atomic_add(overdraw[u32x2(input.position.xy)], 1u, MemoryOrder::Relaxed);
    }

    return VisBufferData(input.meshlet_instance_index, mesh_primitive_id()).encode();
}
