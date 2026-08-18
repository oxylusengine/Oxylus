module meshlet;

import common;
import gpu;
import scene;

#include <defines.slang>

public struct MeshletTaskPayload {
    public u32 base_meshlet_instance_index;
    public u8 visible_indices[CULLING_MESHLET_COUNT];
    public u8 vertex_counts[CULLING_MESHLET_COUNT];
    public u8 triangle_counts[CULLING_MESHLET_COUNT];
};

public func mesh_primitive_id() -> u32 {
    return spirv_asm {
        OpCapability MeshShadingEXT;
        OpExtension "SPV_EXT_mesh_shader";
        OpDecorate builtin(PrimitiveId:u32) Flat;
        result:$$u32 = OpLoad builtin(PrimitiveId:u32);
    };
}
