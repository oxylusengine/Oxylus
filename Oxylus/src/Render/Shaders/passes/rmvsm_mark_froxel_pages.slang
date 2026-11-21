import common;
import gpu;
import scene;
import rmvsm;
import fog;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<VirtualClipmap> clipmaps;
[[vk::binding(2, 0)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables;
[[vk::binding(3, 0)]] RWStructuredBuffer<u32> page_occupancy;
[[vk::binding(4, 0)]] RWStructuredBuffer<VSMPageAllocator> allocator;
[[vk::binding(5, 0)]] Texture2D<f32> tile_depth;

// volumetric fog samples the sun's shadow in the air, where no visible pixel asked for a page. Marks the page under
// the spot each froxel samples this frame, at the level volumetric fog samples. Its center would leave the jittered
// spot on an unbacked page whenever it crosses a page edge, and the coarser fallback flickers in and out
[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 id : SV_DispatchThreadID, uniform VSMContext ctx) -> void {
    let fog = camera.fog;
    let grid = u32x3(u32x2(ceil(camera.resolution / f32(FROXEL_TILE_SIZE))), FROXEL_DEPTH_SLICES);
    if (any(id >= grid) || fog.volumetric_distance <= 0.0) {
        return;
    }

    if (froxel_is_hidden(fog, froxel_column_depth(tile_depth, i32x2(id.xy), i32x2(grid.xy)), id.z)) {
        return;
    }

    let camera_position = camera.position.xyz;
    let world_pos = froxel_world_position(camera, f32x3(id) + fog.volumetric_jitter, grid);

    let clipmap_index = ctx.get_volume_clipmap_index(length(world_pos - camera_position));
    let clipmap = clipmaps[clipmap_index];
    let virt_coords = clipmap.get_clipmap_info(world_pos).get_virt_coords(ctx.get_page_table_extent().xy);
    let wrapped_coords = ctx.page_coords_virtual_to_wrapped(virt_coords, clipmap.page_offset);
    let page_table_address = i32x3(wrapped_coords, clipmap_index);

    // neighbouring froxels share pages, so one lane per distinct page does the atomic, as rmvsm_mark_visible_pages does
    var thread_active = all(wrapped_coords >= 0);
    var first_thread_visible = false;
    while (thread_active) {
        let subgroup_address = WaveReadLaneFirst(page_table_address);
        if (all(subgroup_address == page_table_address)) {
            if (WaveIsFirstLane()) {
                first_thread_visible = true;
            }
            thread_active = false;
        }
    }

    if (!first_thread_visible) {
        return;
    }

    let prev_page = VSMPageMetadata(
        __atomic_or(page_tables[page_table_address], u32(VSMPageState.Visible), MemoryOrder::Relaxed));
    if (prev_page.is_visible()) {
        return;
    }

    if (prev_page.is_backed()) {
        page_occupancy[prev_page.get_physical_address()] = 1u;
    } else {
        push_page_alloc_request(allocator[0], VSMAllocRequest(page_table_address, -1));
    }
}
