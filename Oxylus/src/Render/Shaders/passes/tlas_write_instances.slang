import common;
import gpu;
import scene;

[[vk::binding(0)]] StructuredBuffer<MeshInstance> mesh_instances;
[[vk::binding(1)]] StructuredBuffer<TransformWorld> transforms;
[[vk::binding(2)]] StructuredBuffer<u64> blas_addresses;
[[vk::binding(3)]] RWStructuredBuffer<AccelerationStructureInstance> instances;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    uint3 thread_id : SV_DispatchThreadID,
    uniform u32 instance_count,
) -> void {
    let index = thread_id.x;
    if (index >= instance_count) {
        return;
    }

    let mesh_instance = mesh_instances[index];
    let world = transforms[mesh_instance.transform_index].world;
    let blas_address = blas_addresses[mesh_instance.mesh_index];

    var instance = AccelerationStructureInstance();

    const f32x4 columns[4] = {
        mul(world, f32x4(1.0, 0.0, 0.0, 0.0)),
        mul(world, f32x4(0.0, 1.0, 0.0, 0.0)),
        mul(world, f32x4(0.0, 0.0, 1.0, 0.0)),
        mul(world, f32x4(0.0, 0.0, 0.0, 1.0)),
    };

    [ForceUnroll]
    for (u32 row = 0; row < 3; row++) {
        [ForceUnroll]
        for (u32 col = 0; col < 4; col++) {
            instance.transform[row * 4 + col] = columns[col][row];
        }
    }

    // a skinned instance's BLAS covers the bind pose, so tracing against it would report geometry
    // nowhere near where the character actually is, and it is masked out of the TLAS entirely
    let mask = (blas_address != 0 && !mesh_instance.is_skinned()) ? 0xffu : 0u;
    instance.custom_index_and_mask = (index & 0xffffffu) | (mask << 24);
    instance.sbt_offset_and_flags = 0;
    instance.blas_address = blas_address;

    instances[index] = instance;
}
