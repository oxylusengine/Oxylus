import common;

import visbuffer;
import gpu;
import scene;

struct ShaderParameters {
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<MeshInstance> mesh_instances;
    Image2D<u32> visbuffer;
};

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main(
  uniform ParameterBlock<ShaderParameters> params,
  uniform u32x2 texel,
  uniform u32 *transform_id
) -> void {
    let texel = params.visbuffer.load(texel);
    if (texel == ~0u) {
        *transform_id = ~0u;
        return;
    }

    let vis = VisBufferData(texel);
    let meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
    let mesh_instance = params.mesh_instances[meshlet_instance.mesh_instance_index];
    *transform_id = mesh_instance.transform_index;
}
