import common;
import gpu;
import particles;

struct PushConstants {
    u32 sorted_count;
};
[[vk::push_constant]] PushConstants C;

[[vk::binding(0, 0)]] RWStructuredBuffer<ParticleSortKey> sort_keys;
[[vk::binding(1, 0)]] StructuredBuffer<ParticleCounters> counters;

groupshared ParticleSortKey lds[256];

[[shader("compute")]]
[[numthreads(256, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID, u32 local_index : SV_GroupIndex) -> void {
    let global_index = thread_id.x;
    let alive = counters[0].alive_count_next;

    ParticleSortKey entry;
    entry.key = 0xFFFFFFFFu;
    entry.index = 0xFFFFFFFFu;
    if (global_index < alive) {
        entry = sort_keys[global_index];
    }

    lds[local_index] = entry;
    GroupMemoryBarrierWithGroupSync();

    for (u32 k = 2; k <= 256; k <<= 1) {
        for (u32 j = k >> 1; j > 0; j >>= 1) {
            let partner = local_index ^ j;
            if (partner > local_index) {
                let a = lds[local_index];
                let b = lds[partner];
                let ascending = (global_index & k) == 0;
                if ((a.key > b.key) == ascending) {
                    lds[local_index] = b;
                    lds[partner] = a;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    if (global_index < C.sorted_count) {
        sort_keys[global_index] = lds[local_index];
    }
}
