import common;

import visbuffer;
import gpu;
import scene;

struct ShaderParameters {
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<MeshInstance> mesh_instances;
    Texture2D<u32> visbuffer;
    Texture2D<u32> refractor_visbuffer;
    StructuredBuffer<MeshletInstance> transparent_meshlet_instances;
};

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main(
  uniform ParameterBlock<ShaderParameters> params,
  uniform u32x2 texel,
  uniform u32 *transform_id,
  uniform u32 terrain_transform_index
) -> void {
    // glass and water sit in front of whatever the main visbuffer holds there
    var refractor_width = 0u;
    var refractor_height = 0u;
    var refractor_levels = 0u;
    params.refractor_visbuffer.GetDimensions(0, refractor_width, refractor_height, refractor_levels);
    let is_inside_refractors = all(texel < u32x2(refractor_width, refractor_height));
    let refractor_texel = is_inside_refractors ? params.refractor_visbuffer.Load(u32x3(texel, 0)) : ~0u;
    if (refractor_texel != ~0u) {
        let vis = VisBufferData(refractor_texel);
        let meshlet_instance = params.transparent_meshlet_instances[vis.meshlet_instance_index];
        *transform_id = params.mesh_instances[meshlet_instance.mesh_instance_index].transform_index;
        return;
    }

    let texel = params.visbuffer.Load(u32x3(texel, 0));
    if (texel == ~0u) {
        *transform_id = ~0u;
        return;
    }

    if (is_terrain(texel)) {
        *transform_id = terrain_transform_index;
        return;
    }

    let vis = VisBufferData(texel);
    let meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];
    *transform_id = mesh_instance.transform_index;
}
