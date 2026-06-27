import common;
import gpu;
import rmvsm;

[[vk::binding(0)]] RWStructuredBuffer<VSMPageAllocator> allocator;
[[vk::binding(1)]] StructuredBuffer<u32> page_occupancy;

[[shader("compute")]]
[[numthreads(64, 1, 1)]]
func cs_main(
    u32 thread_id : SV_DispatchThreadID,
    uniform u32 physical_page_count,
) -> void {
    let is_free = thread_id < physical_page_count && page_occupancy[thread_id] == 0u;

    let local_offset = WavePrefixCountBits(is_free);
    let wave_total = WaveActiveCountBits(is_free);

    var wave_base = 0u;
    if (WaveIsFirstLane()) {
        wave_base = reserve_free_page_slots(allocator[0], wave_total);
    }
    wave_base = WaveReadLaneFirst(wave_base);

    if (!is_free) {
        return;
    }

    allocator[0].free_page_list[wave_base + local_offset] = thread_id;
}
