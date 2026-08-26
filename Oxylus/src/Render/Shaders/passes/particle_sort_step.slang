import common;
import gpu;
import particles;

struct PushConstants {
    u32 sorted_count;
    u32 k;
    u32 j;
};
[[vk::push_constant]] PushConstants C;

[[vk::binding(0, 0)]] RWStructuredBuffer<ParticleSortKey> sort_keys;

[[shader("compute")]]
[[numthreads(256, 1, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let index = thread_id.x;
    if (index >= C.sorted_count) {
        return;
    }

    let partner = index ^ C.j;
    if (partner <= index || partner >= C.sorted_count) {
        return;
    }

    let a = sort_keys[index];
    let b = sort_keys[partner];
    let ascending = (index & C.k) == 0;

    if ((a.key > b.key) == ascending) {
        sort_keys[index] = b;
        sort_keys[partner] = a;
    }
}
