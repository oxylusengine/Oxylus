import common;
import gpu;
import scene;
import cull;
import visbuffer;
import meshlet;

#include <transparent_forward_common.slang>

[[vk::constant_id(2)]] const u32 CULL_FLAGS;

[[vk::binding(14, 0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(15, 0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(16, 0)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(17, 0)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(18, 0)]] Image2D<f32> hiz_image;
[[vk::binding(19, 0)]] StructuredBuffer<MeshletInstanceVisibility> visibility;

struct MeshForwardPush {
    CullCamera *cull_camera;
    ForwardPush forward;
};
[[vk::push_constant]] MeshForwardPush push;

struct PrimitiveOutput {
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
    let cull_camera = *push.cull_camera;
    let list = meshlet_task_list(CULL_FLAGS, cull_camera.meshlet_instance_capacity, visibility);
    let meshlet_instance_index = list.offset + dispatch_thread_id;

    if (local_index == 0) {
        visible_meshlet_count = 0;
        payload.base_meshlet_instance_index = meshlet_instance_index;
    }

    GroupMemoryBarrierWithGroupSync();

    if (dispatch_thread_id < list.count) {
        MeshletCullResources cull_resources;
        cull_resources.hiz_image = hiz_image;
        cull_resources.meshes = meshes;
        cull_resources.mesh_instances = mesh_instances;
        cull_resources.meshlet_instances = meshlet_instances;
        cull_resources.transforms = transforms;
        cull_resources.materials = materials;
        cull_resources.camera = cull_camera;

        if (is_task_meshlet_visible(cull_resources, CULL_FLAGS, meshlet_instance_index, true)) {
            let slot = __atomic_add(visible_meshlet_count, 1, MemoryOrder::Relaxed);
            payload.visible_indices[slot] = u8(local_index);
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
    OutputVertices<ForwardVertex, MESHLET_MAX_VERTICES> vertices,
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

    if (local_index < vertex_count) {
        let vertex_index = source.mesh_lod.indirect_vertex_indices[source.meshlet.indirect_vertex_index_offset + local_index];
        let vertex = fetch_meshlet_vertex(source, vertex_index);
        let clip_pos = camera.jitter_clip(mul(camera.projection_view, f32x4(vertex.world_position, 1.0)));
        clip_positions_shared[local_index] = clip_pos;

        ForwardVertex output;
        output.position = clip_pos;
        output.world_position = vertex.world_position;
        output.world_normal = vertex.world_normal;
        output.tex_coord = vertex.tex_coord;
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
        output.culled = is_meshlet_triangle_culled(clip_pos, is_double_sided_shared);
        primitives[local_index] = output;
    }
}

[[shader("fragment")]]
func fs_main(ForwardVertex input, bool is_front_face : SV_IsFrontFace) -> ForwardOutput {
    return shade_transparent(input, is_front_face, push.forward);
}
