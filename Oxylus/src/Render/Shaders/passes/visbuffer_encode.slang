module visbuffer_encode;

import common;
import gpu;
import scene;
import visbuffer;

struct ShaderParameters {
    ConstantBuffer<Camera> camera;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<MeshInstance> mesh_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;
    StructuredBuffer<Material> materials;
    StorageImage2D<u32, ImageFormat::R32UI> overdraw;
};
ParameterBlock<ShaderParameters> params;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x3 vertex_pos : POSITION;
    f32x2 tex_coord : TEXCOORD;
    nointerpolation u32 meshlet_instance_index;
    nointerpolation u32 triangle_index;
    nointerpolation u32 material_index;
};

[[shader("vertex")]]
func vs_main(u32 vertex_index : SV_VertexID) -> VertexOutput {
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
    let clip_pos = mul(params.camera.projection_view, f32x4(world_pos.xyz, 1.0));

    VertexOutput output;
    output.position = clip_pos;
    output.vertex_pos = vertex_pos;
    output.tex_coord = tex_coord;
    output.meshlet_instance_index = vis.meshlet_instance_index;
    output.triangle_index = vis.triangle_index / 3;
    output.material_index = mesh_instance.material_index;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> u32 {
#if 1
    let material = params.materials[input.material_index];
    if (material.flags & MaterialFlag::HasAlbedoImage) {
        UVGradient grad;
        grad.uv = input.tex_coord;
        grad.ddx = ddx(input.tex_coord);
        grad.ddy = ddy(input.tex_coord);
        let alpha_color = material.sample_albedo_color(grad).a;

        // We are doing deferred, blend alpha mode is not supported in this pass.
        if (alpha_color < clamp(material.alpha_cutoff, 0.001, 1.0)) {
            discard;
        }
    }
#endif

    com::atomic_add(params.overdraw[u32x2(input.position.xy)], 1u, com::memory_order_acq_rel, com::MemoryLocation::Image, MemoryScope::QueueFamily);

    let vis = VisBufferData(input.meshlet_instance_index, input.triangle_index);
    return vis.encode();
}
