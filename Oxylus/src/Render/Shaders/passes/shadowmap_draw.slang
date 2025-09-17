module visbuffer_encode;

import common;
import gpu;
import scene;
import visbuffer;

struct ShaderParameters {
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<MeshInstance> mesh_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;
};
ParameterBlock<ShaderParameters> params;

struct VertexOutput {
    f32x4 position : SV_Position;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID, uniform f32x4x4 projection_view) -> VertexOutput {
    let vis = VisBufferData(vertex_index);
    let meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];
    let mesh = params.meshes[mesh_instance.mesh_index];
    let mesh_lod = mesh.lods[mesh_instance.lod_index];
    let transform = params.transforms[mesh_instance.transform_index];
    let meshlet = mesh_lod.meshlets[meshlet_instance.meshlet_index];

    let index = meshlet.index(mesh_lod, vis.triangle_index);
    let vertex_pos = meshlet.position(mesh, index);
    let tex_coord = meshlet.tex_coord(mesh, index);
    let world_pos = transform.to_world_position(vertex_pos);
    let clip_pos = mul(projection_view, f32x4(world_pos.xyz, 1.0));

    VertexOutput output;
    output.position = clip_pos;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    return f32x4(1.0);
}
