import common;
import gpu;

struct ShaderParameters {
    StructuredBuffer<u32> meshlet_instances_count;

    RWStructuredBuffer<DispatchIndirectCommand> cull_meshlets_cmd;
};

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main(
    uniform ParameterBlock<ShaderParameters> params
) -> void {
    params.cull_meshlets_cmd[0].x = (params.meshlet_instances_count[0] + (CULLING_MESHLET_COUNT - 1)) / CULLING_MESHLET_COUNT;
}

