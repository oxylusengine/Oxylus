import common;
import gpu;
import particles;

struct PushConstants {
    u32 sorted_count;
    u32 k;
};
[[vk::push_constant]] PushConstants C;

[[vk::binding(0, 0)]] RWStructuredBuffer<ParticleSortKey> sort_keys;

groupshared ParticleSortKey lds[256];

[[shader("compute")]]
[[numthreads(256, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, u32 local_index : SV_GroupIndex) -> void {
    let global_index = thread_id.x;

    ParticleSortKey entry;
    entry.key = 0xFFFFFFFFu;
    entry.index = 0xFFFFFFFFu;
    if (global_index < C.sorted_count) {
        entry = sort_keys[global_index];
    }

    lds[local_index] = entry;
    GroupMemoryBarrierWithGroupSync();

    let ascending = (global_index & C.k) == 0;

    for (u32 j = 128; j > 0; j >>= 1) {
        let partner = local_index ^ j;
        if (partner > local_index) {
            let a = lds[local_index];
            let b = lds[partner];
            if ((a.key > b.key) == ascending) {
                lds[local_index] = b;
                lds[partner] = a;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (global_index < C.sorted_count) {
        sort_keys[global_index] = lds[local_index];
    }
}
