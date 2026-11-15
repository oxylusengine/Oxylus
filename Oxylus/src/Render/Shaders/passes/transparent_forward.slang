import common;
import gpu;
import scene;
import visbuffer;
import meshlet;

#include <transparent_forward_common.slang>

[[vk::binding(14, 0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(15, 0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(16, 0)]] StructuredBuffer<MeshletInstance> meshlet_instances;
[[vk::binding(17, 0)]] StructuredBuffer<TransformWorld> transforms;

[[vk::push_constant]] ForwardPush push;

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID) -> ForwardVertex {
    let vis = VisBufferData(vertex_index);
    let source = resolve_meshlet_source(meshes, mesh_instances, transforms, meshlet_instances[vis.meshlet_instance_index]);
    let vertex = fetch_meshlet_vertex(source, source.meshlet.index(source.mesh_lod, vis.triangle_index));

    ForwardVertex output;
    output.position = camera.jitter_clip(mul(camera.projection_view, f32x4(vertex.world_position, 1.0)));
    output.world_position = vertex.world_position;
    output.world_normal = vertex.world_normal;
    output.tex_coord = vertex.tex_coord;
    output.material_index = source.mesh_instance.material_index;

    return output;
}

[[shader("fragment")]]
func fs_main(ForwardVertex input, bool is_front_face : SV_IsFrontFace) -> ForwardOutput {
    return shade_transparent(input, is_front_face, push);
}
