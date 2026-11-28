import common;
import gpu;
import scene;

[[vk::binding(0)]] StructuredBuffer<Mesh> meshes;
[[vk::binding(1)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(2)]] StructuredBuffer<SkinningTransform> skinning_transforms;
[[vk::binding(3)]] RWStructuredBuffer<u32x2> skinned_positions;
[[vk::binding(4)]] RWStructuredBuffer<u32> skinned_normals;
[[vk::binding(5)]] StructuredBuffer<SkinJob> skin_jobs;

[[shader("compute")]]
[[numthreads(SKIN_GROUP_SIZE, 1, 1)]]
func cs_main(u32x3 group_id: SV_GroupID, u32x3 local_id: SV_GroupThreadID, uniform SkinPush push) -> void {
    let group = group_id.y * push.groups_x + group_id.x;

    // the last job starting at or before this workgroup, jobs are sorted by `first_group`
    var low = u32(0);
    var high = push.job_count - 1;
    while (low < high) {
        let middle = (low + high + 1) / 2;
        if (skin_jobs[middle].first_group <= group) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }

    let job = skin_jobs[low];
    let vertex_index = (group - job.first_group) * SKIN_GROUP_SIZE + local_id.x;
    if (group < job.first_group || vertex_index >= job.vertex_count) {
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
        // a joint past the animator's skeleton would read another instance's bones, or past the end
        let joint = u32(joints[i]);
        if (weight <= 0.0 || joint >= job.bone_count) {
            continue;
        }

        let bone = skinning_transforms[job.bone_offset + joint];
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
