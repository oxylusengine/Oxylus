import common;
import gpu;
import scene;

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<SkinningTransform> skinning_transforms;
[[vk::binding(3)]] RWStructuredBuffer<u32x2> skinned_positions;
[[vk::binding(4)]] RWStructuredBuffer<u32> skinned_normals;

struct SkinJob {
    u32 mesh_instance_index;
    u32 vertex_offset;
    u32 bone_offset;
    u32 vertex_count;
};

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(u32x3 thread_id: SV_DispatchThreadID, uniform SkinJob job) -> void {
    let vertex_index = thread_id.x;
    if (vertex_index >= job.vertex_count) {
        return;
    }

    let mesh_instance = mesh_instances[job.mesh_instance_index];
    let mesh = meshes[mesh_instance.mesh_index];
    if (mesh.skin_joint_indices == nullptr || mesh.skin_weights == nullptr) {
        return;
    }

    let bind_position = mesh.decode_position(vertex_index);
    let bind_normal = mesh.decode_normal(vertex_index);

    let joints = mesh.skin_joint_indices[vertex_index];
    let weights = mesh.skin_weights[vertex_index];

    var position = f32x3(0.0);
    var normal = f32x3(0.0);

    [unroll]
    for (u32 i = 0; i < 4; ++i) {
        let weight = f32(weights[i]) / 65535.0;
        if (weight <= 0.0) {
            continue;
        }

        let bone = skinning_transforms[job.bone_offset + u32(joints[i])];
        position += bone.transform_point(bind_position) * weight;
        normal += bone.transform_normal(bind_normal) * weight;
    }

    normal = normalize(normal);

    let out_index = job.vertex_offset + vertex_index;
    skinned_positions[out_index] = u32x2(
        u32(com::quantize_half(position.x)) | (u32(com::quantize_half(position.y)) << 16),
        u32(com::quantize_half(position.z))
    );

    // meshopt_quantizeSnorm rounds half away from zero, so a plain +0.5 bias would be off by one
    // for negative components and the skinned normals would not match the bind-pose encoding
    let scaled = clamp(normal, -1.0, 1.0) * 511.0;
    let bias = f32x3(
        scaled.x >= 0.0 ? 0.5 : -0.5,
        scaled.y >= 0.0 ? 0.5 : -0.5,
        scaled.z >= 0.0 ? 0.5 : -0.5
    );
    let quantized_normal = (i32x3(scaled + bias) + i32x3(511)) & i32x3(1023);
    skinned_normals[out_index] = (u32(quantized_normal.x) << 20) | (u32(quantized_normal.y) << 10) |
                                 u32(quantized_normal.z);
}
