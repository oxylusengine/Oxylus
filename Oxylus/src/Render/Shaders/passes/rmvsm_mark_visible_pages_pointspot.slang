import common;
import gpu;
import scene;
import rmvsm;

#include <rmvsm_pointspot.slang>

[[vk::binding(0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1)]] StructuredBuffer<VSMPointSpotView> views;
[[vk::binding(2)]] StructuredBuffer<u32x4> light_grid;

[[vk::binding(3)]] Texture2D<f32> depth_image;

[[vk::binding(4)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip0;
[[vk::binding(5)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip1;
[[vk::binding(6)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip2;
[[vk::binding(7)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip3;
[[vk::binding(8)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip4;
[[vk::binding(9)]] RWTexture2DArray<u32, ImageFormat::R32UI> page_tables_mip5;

[[vk::binding(10)]] RWStructuredBuffer<u32> page_occupancy;
[[vk::binding(11)]] RWStructuredBuffer<VSMPageAllocator> allocator;

func atomic_or_page(i32x3 address, i32 mip, u32 value) -> u32 {
    switch (mip) {
        case 0: return __atomic_or(page_tables_mip0[address], value, MemoryOrder::Relaxed);
        case 1: return __atomic_or(page_tables_mip1[address], value, MemoryOrder::Relaxed);
        case 2: return __atomic_or(page_tables_mip2[address], value, MemoryOrder::Relaxed);
        case 3: return __atomic_or(page_tables_mip3[address], value, MemoryOrder::Relaxed);
        case 4: return __atomic_or(page_tables_mip4[address], value, MemoryOrder::Relaxed);
        default: return __atomic_or(page_tables_mip5[address], value, MemoryOrder::Relaxed);
    }
}

func mark_page(i32x3 address, i32 mip) -> void {
    let prev_page_data = atomic_or_page(address, mip, u32(VSMPageState.Visible));
    let prev_page = VSMPageMetadata(prev_page_data);

    if (!prev_page.is_visible()) {
        // page became visible this frame

        if (prev_page.is_backed()) {
            // keep track of visible and backed pages so the allocator wont overwrite it
            let physical_address = prev_page.get_physical_address();
            page_occupancy[physical_address] = 1u;
        } else {
            let alloc_request = VSMAllocRequest(address, mip);
            push_page_alloc_request(allocator[0], alloc_request);
        }
    }
}

func mark_view(
    in VSMPointSpotContext ctx,
    i32 layer,
    f32x3 world_pos,
    f32x3 world_pos_left,
    f32x3 world_pos_right
) -> void {
    let view = views[layer];
    if (view.range <= 0.0) {
        return;
    }

    let center = view.project(world_pos);
    if (!center.valid) {
        return;
    }

    let uv_left = view.project(world_pos_left).uv;
    let uv_right = view.project(world_pos_right).uv;
    let mip = vsm_pointspot_mip(uv_left, uv_right, ctx.mip_bias_min);
    let page_coords = vsm_pointspot_page_coords(center.uv, mip);

    mark_page(i32x3(page_coords, layer), mip);
}

func active_slot_mask(u32 count) -> u32x2 {
    let lo = count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u);
    let hi = count <= 32u ? 0u : (count >= 64u ? 0xFFFFFFFFu : ((1u << (count - 32u)) - 1u));
    return u32x2(lo, hi);
}

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(
    u32x2 thread_id : SV_DispatchThreadID,
    uniform VSMPointSpotContext ctx,
    uniform f32x3 light_grid_origin
) -> void {
    if (any(thread_id >= u32x2(ctx.depth_extent))) {
        return;
    }

    let depth = depth_image.Load(i32x3(thread_id, 0));
    if (depth == 0.0) {
        return;
    }

    let inv_resolution = 1.0 / f32x2(ctx.depth_extent);
    let uv = (f32x2(thread_id) + 0.5) * inv_resolution;
    let world_pos = camera.unproject_uv(uv, depth);

    let uv_offset = inv_resolution * 0.5;
    let world_pos_left = camera.unproject_uv(uv + f32x2(-uv_offset.x, uv_offset.y), depth);
    let world_pos_right = camera.unproject_uv(uv + f32x2(uv_offset.x, uv_offset.y), depth);

    let grid_cell = light_grid_cell_coords(world_pos, light_grid_origin);
    var point_mask = u32x2(0);
    var spot_mask = u32x2(0);
    if (light_grid_cell_in_bounds(grid_cell)) {
        let cell_mask = light_grid[light_grid_cell_index(grid_cell)];
        point_mask = cell_mask.xy;
        spot_mask = cell_mask.zw;
    } else {
        point_mask = active_slot_mask(ctx.shadow_point_light_count);
        spot_mask = active_slot_mask(ctx.shadow_spot_light_count);
    }

    for (var word = 0; word < 2; word++) {
        var mask = point_mask[word];
        while (mask != 0u) {
            let slot = word * 32 + i32(firstbitlow(mask));
            mask &= mask - 1u;

            let base_layer = slot * 6;
            let view = views[base_layer];
            if (view.range <= 0.0) {
                continue;
            }

            let to_pixel = world_pos - view.light_position;
            if (dot(to_pixel, to_pixel) > view.range * view.range) {
                continue;
            }

            let face_index = vsm_point_face_index(to_pixel);
            mark_view(ctx, base_layer + face_index, world_pos, world_pos_left, world_pos_right);
        }
    }

    for (var word = 0; word < 2; word++) {
        var mask = spot_mask[word];
        while (mask != 0u) {
            let slot = word * 32 + i32(firstbitlow(mask));
            mask &= mask - 1u;

            let layer = vsm_spot_layer(slot);
            let view = views[layer];
            if (view.range <= 0.0) {
                continue;
            }

            let to_pixel = world_pos - view.light_position;
            if (dot(to_pixel, to_pixel) > view.range * view.range) {
                continue;
            }

            mark_view(ctx, layer, world_pos, world_pos_left, world_pos_right);
        }
    }
}
