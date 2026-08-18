import common;
import gpu;

struct ShaderParameters {
    StructuredBuffer<DispatchIndirectCommand> source_command;
    StructuredBuffer<u32> clipmap_dirty_flags;
    RWStructuredBuffer<DispatchIndirectCommand> draw_commands;
    RWStructuredBuffer<u32> draw_count;
    RWStructuredBuffer<u32> draw_clipmaps;
};

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main(
    uniform ParameterBlock<ShaderParameters> params,
    uniform u32 clipmap_count
) -> void {
    var active_count = 0u;

    for (var reverse_index = 0u; reverse_index < clipmap_count; reverse_index++) {
        let clipmap_index = clipmap_count - 1u - reverse_index;
        if (params.clipmap_dirty_flags[clipmap_index] != 0u) {
            params.draw_commands[active_count] = params.source_command[0];
            params.draw_clipmaps[active_count] = clipmap_index;
            active_count++;
        }
    }

    params.draw_count[0] = active_count;
}
