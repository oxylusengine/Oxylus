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
    f32x2 tex_coord;
    nointerpolation u32 meshlet_instance_index;
    nointerpolation u32 material_index;
};

struct PrimitiveOutput {
    u32 triangle_index : SV_PrimitiveID;
    bool culled : SV_CullPrimitive;
};

groupshared u32 visible_meshlet_count;
groupshared MeshletTaskPayload payload;

[[shader("task")]]
[[numthreads(CULLING_MESHLET_COUNT, 1, 1)]]
func ts_main(
    u32 dispatch_thread_id : SV_DispatchThreadID,
    u32 local_index : SV_GroupIndex
) -> void {
    let is_transparent = HAS_FLAG(CULL_FLAGS, CullFlag.Transparent);
    let list = meshlet_task_list(CULL_FLAGS, camera.meshlet_instance_capacity, visibility);
    let meshlet_instance_index = list.offset + dispatch_thread_id;

    if (local_index == 0) {
        visible_meshlet_count = 0;
        payload.base_meshlet_instance_index = meshlet_instance_index;
    }

    GroupMemoryBarrierWithGroupSync();

    if (dispatch_thread_id < list.count) {
        var visibility_mask_index = 0u;
        var visibility_bit = 0u;
        var was_visible = true;
        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestOcclusion) && !is_transparent) {
            let meshlet_instance = meshlet_instances[meshlet_instance_index];
            let mesh_instance = mesh_instances[meshlet_instance.mesh_instance_index];
            let meshlet_instance_visibility_index = mesh_instance.meshlet_instance_visibility_offset + meshlet_instance.meshlet_index;
            visibility_mask_index = meshlet_instance_visibility_index / 32;
            let bit_index = meshlet_instance_visibility_index - visibility_mask_index * 32;
            visibility_bit = 1u << bit_index;
            was_visible = (meshlet_instance_visibility_mask[visibility_mask_index] & visibility_bit) != 0;
        }

        MeshletCullResources cull_resources;
        cull_resources.hiz_image = hiz_image;
        cull_resources.meshes = meshes;
        cull_resources.mesh_instances = mesh_instances;
        cull_resources.meshlet_instances = meshlet_instances;
        cull_resources.transforms = transforms;
        cull_resources.materials = materials;
        cull_resources.camera = camera;
        let visible = is_task_meshlet_visible(cull_resources, CULL_FLAGS, meshlet_instance_index, was_visible);

        if (visible && (!HAS_FLAG(CULL_FLAGS, CullFlag.LatePass) || !was_visible)) {
            let slot = __atomic_add(visible_meshlet_count, 1, MemoryOrder::Relaxed);
            payload.visible_indices[slot] = u8(local_index);
        }

        if (HAS_FLAG(CULL_FLAGS, CullFlag.TestOcclusion | CullFlag.LatePass) && !is_transparent) {
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
groupshared bool is_double_sided_shared;

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
    let meshlet_instance_index = payload.base_meshlet_instance_index + u32(payload.visible_indices[slot]);
    let source = resolve_meshlet_source(meshes, mesh_instances, transforms, meshlet_instances[meshlet_instance_index]);
    let vertex_count = u32(source.meshlet.vertex_count);
    let triangle_count = u32(source.meshlet.triangle_count);
    SetMeshOutputCounts(vertex_count, triangle_count);

    if (local_index == 0) {
        is_double_sided_shared = materials[source.mesh_instance.material_index].is_double_sided();
    }

    let mvp = mul(camera.projection_view, source.transform.world);

    if (local_index < vertex_count) {
        let vertex_index = source.mesh_lod.indirect_vertex_indices[source.meshlet.indirect_vertex_index_offset + local_index];
        let clip_pos = camera.jitter_clip(mul(mvp, f32x4(source.meshlet.position(source.mesh, vertex_index), 1.0)));
        clip_positions_shared[local_index] = clip_pos;

        VertexOutput output;
        output.position = clip_pos;
        output.tex_coord = source.meshlet.tex_coord(source.mesh, vertex_index);
        output.meshlet_instance_index = meshlet_instance_index;
        output.material_index = source.mesh_instance.material_index;
        vertices[local_index] = output;
    }

    GroupMemoryBarrierWithGroupSync();

    if (local_index < triangle_count) {
        let corners = meshlet_triangle_corners(source, local_index);
        triangles[local_index] = corners;

        let clip_pos = f32x4x3(
            clip_positions_shared[corners.x],
            clip_positions_shared[corners.y],
            clip_positions_shared[corners.z]
        );

        PrimitiveOutput output;
        output.triangle_index = local_index;
        output.culled = is_meshlet_triangle_culled(clip_pos, is_double_sided_shared);
        primitives[local_index] = output;
    }
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> u32 {
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

    if (RENDER_OVERDRAW) {
        __atomic_add(overdraw[u32x2(input.position.xy)], 1u, MemoryOrder::Relaxed);
    }

    return VisBufferData(input.meshlet_instance_index, mesh_primitive_id()).encode();
}
