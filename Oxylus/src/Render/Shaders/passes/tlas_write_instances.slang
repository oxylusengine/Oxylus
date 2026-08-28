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
    uniform u64 skinned_blas_address_min,
    uniform u64 skinned_blas_address_max,
    uniform u32 instance_count,
    uniform u32 blas_address_count,
) -> void {
    let index = thread_id.x;
    if (index >= instance_count) {
        return;
    }

    let mesh_instance = mesh_instances[index];
    let world = transforms[mesh_instance.transform_index].world;
    // the per-mesh BLAS is the bind pose, which is nowhere near where an animated character
    // actually is, so a skinned instance traces against the one rebuilt from its skinned vertices
    let skinned = mesh_instance.is_skinned();
    // the instance array and the address array are uploaded on separate schedules, so an instance
    // can name a mesh the address array does not have yet. Reading past its end returns whatever
    // the allocation holds, and the build takes that as a structure handle
    u64 blas_address = 0;
    if (skinned) {
        blas_address = mesh_instance.skinned_blas_address;
    } else if (mesh_instance.mesh_index < blas_address_count) {
        blas_address = blas_addresses[mesh_instance.mesh_index];
    }

    // the build reads a structure reference as a raw address, so an instance array that lags the
    // pool by a frame costs a lost device rather than a wrong pixel. Only an address the pool
    // actually minted this frame gets through, and dropping the rest costs one frame of a character
    if (skinned && (blas_address < skinned_blas_address_min || blas_address > skinned_blas_address_max)) {
        blas_address = 0;
    }

    // cheap catch for the static path, whose addresses come from a buffer with the same staleness
    // window: every structure is allocated at a 256 byte offset, so an odd one is not one of ours
    if ((u32(blas_address) & 0xffu) != 0u) {
        blas_address = 0;
    }

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

    let mask = blas_address != 0 ? 0xffu : 0u;
    instance.custom_index_and_mask = (index & 0xffffffu) | (mask << 24);
    instance.sbt_offset_and_flags = 0;
    instance.blas_address = blas_address;

    instances[index] = instance;
}
