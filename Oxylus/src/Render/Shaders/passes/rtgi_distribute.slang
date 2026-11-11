import common;
import gpu;
import scene;
import rtgi_common;

[[vk::binding(0, 0)]] [[format("r16ui")]] Texture2D<u32> sample_count_image;
[[vk::binding(1, 0)]] Texture2D<f32x4> ray_impact_image;
[[vk::binding(2, 0)]] [[format("r8ui")]] RWTexture2D<u32> ray_count_image;
[[vk::binding(3, 0)]] RWTexture2D<u32> pixel_ray_alloc_image;
[[vk::binding(4, 0)]] RWStructuredBuffer<RTGIRayEntry> ray_list;
[[vk::binding(5, 0)]] RWStructuredBuffer<RTGIRayCounters> counters;

static const u32 TILE_SIZE = 8;
static const u32 TILE_THREADS = 64;
static const f32 GOLDEN_RATIO_FRACTION = 0.61803398875;

groupshared u32 gs_scan[2][TILE_THREADS];
groupshared u32 gs_base_offset;

// the scan runs in groupshared memory so it does not depend on the wave size, every thread of the tile must call it
func tile_inclusive_scan(u32 thread_index, u32 value, out u32 total) -> u32 {
    gs_scan[0][thread_index] = value;
    GroupMemoryBarrierWithGroupSync();

    u32 source = 0u;
    for (u32 offset = 1u; offset < TILE_THREADS; offset <<= 1u) {
        let previous = gs_scan[source][thread_index - min(offset, thread_index)];
        let sum = gs_scan[source][thread_index] + select(thread_index >= offset, previous, 0u);
        gs_scan[source ^ 1u][thread_index] = sum;
        source ^= 1u;
        GroupMemoryBarrierWithGroupSync();
    }

    total = gs_scan[source][TILE_THREADS - 1u];
    let inclusive = gs_scan[source][thread_index];
    GroupMemoryBarrierWithGroupSync();
    return inclusive;
}

[[shader("compute")]]
[numthreads(8, 8, 1)]
func cs_main(
    u32x2 half_pixel : SV_DispatchThreadID,
    u32x2 group_thread : SV_GroupThreadID,
    u32x2 group_id : SV_GroupID,
    uniform RTGISettings settings
) {
    let thread_index = group_thread.y * TILE_SIZE + group_thread.x;
    let is_in_bounds = all(half_pixel < settings.half_size);

    var desired = 0u;
    if (is_in_bounds) {
        let history_count = rtgi_unpack_normal_count(sample_count_image.Load(u32x3(half_pixel, 0u)));
        let is_geometry = history_count >= 0.0;
        if (is_geometry) {
            let impact_diffuse = rtgi_unpack_ray_impact_factor(ray_impact_image.Load(u32x3(half_pixel, 0u)).x);
            desired = rtgi_calc_ray_demand(history_count, impact_diffuse, settings);
        }
    }

    let base_ray = u32(desired > 0u);
    let extra_ray = desired - base_ray;

    let total_budget = rtgi_hard_ray_budget(settings.half_size, settings);
    let total_geo = counters[0].total_geo_rays;
    let total_extra = counters[0].total_extra_rays;
    let is_draining = total_budget < total_geo + total_extra;

    var actual = desired;
    if (is_draining) {
        let min_budget = clamp(settings.min_ray_budget, 0.0, 1.0);
        let frame = f32(settings.frame_index);

        // the base dither can round up by one pixel per tile, that slack is reserved up front
        let tile_count = ((settings.half_size.x + TILE_SIZE - 1u) / TILE_SIZE) * ((settings.half_size.y + TILE_SIZE - 1u) / TILE_SIZE);
        let split_budget = max(total_budget, 2u * tile_count) - 2u * tile_count;
        let min_base_spend = min(f32(total_geo) * min_budget, f32(split_budget));
        let global_extra_ratio = select(
            total_extra > 0u,
            saturate((f32(split_budget) - min_base_spend) / f32(max(total_extra, 1u))),
            0.0
        );
        let extra_spend = f32(total_extra) * global_extra_ratio;
        let geo_rcp = 1.0 / f32(max(total_geo, 1u));
        let global_base_fraction = select(
            total_geo > 0u,
            min(min(max((f32(split_budget) - extra_spend) * geo_rcp, min_budget), 1.0), f32(split_budget) * geo_rcp),
            0.0
        );
        let tile_id = group_id.y * ((settings.half_size.x + TILE_SIZE - 1u) / TILE_SIZE) + group_id.x;

        // base count in the low byte (at most 64), extra rays above it (at most 64 * 11) so one scan carries both
        u32 tile_totals;
        let inclusive = tile_inclusive_scan(thread_index, (extra_ray << 8u) | base_ray, tile_totals);
        let exclusive = inclusive - ((extra_ray << 8u) | base_ray);
        let tile_base_count = tile_totals & 0xffu;
        let tile_extra_demand = tile_totals >> 8u;

        let tile_base_budget = min(u32(f32(tile_base_count) * global_base_fraction), tile_base_count);
        let tile_extra_budget = min(u32(f32(tile_extra_demand) * global_extra_ratio), tile_extra_demand);

        // base rays go to the pixels whose Bayer rank is under the tile's base fraction, rotated every frame
        let base_fraction = f32(tile_base_budget) / f32(max(tile_base_count, 1u));
        let bayer = f32(com::bayer_8x8(group_thread)) * (1.0 / 64.0);
        let is_base_won = and(base_ray > 0u, frac(bayer + frac(frame * GOLDEN_RATIO_FRACTION)) < base_fraction);

        // extras follow a cumulative scan weighted by each pixel's extra demand, the phase rotates per frame and tile
        let extra_phase = frac(frame * GOLDEN_RATIO_FRACTION + f32(tile_id) * 0.7548776662 + 0.5);
        let inv_extra_demand = 1.0 / f32(max(tile_extra_demand, 1u));
        let extra_budget = f32(tile_extra_budget);
        // a tile without extra demand has an extra budget of 0, which clamps both targets to 0
        let target_before = min(u32(saturate(f32(exclusive >> 8u) * inv_extra_demand) * extra_budget + extra_phase), tile_extra_budget);
        let target_after = min(u32(saturate(f32(inclusive >> 8u) * inv_extra_demand) * extra_budget + extra_phase), tile_extra_budget);

        let extra_alloc = min(target_after - target_before, extra_ray);
        actual = select(is_base_won, base_ray, 0u) + extra_alloc;
    }

    u32 tile_total;
    let actual_inclusive = tile_inclusive_scan(thread_index, actual, tile_total);
    if (thread_index == 0u) {
        u32 old_count;
        InterlockedAdd(counters[0].ray_list_count, tile_total, old_count);
        gs_base_offset = old_count;
    }

    GroupMemoryBarrierWithGroupSync();

    if (!is_in_bounds) {
        return;
    }

    // rays past the limit are dropped, the trace never reads past it either
    let ray_list_limit = rtgi_ray_list_limit(settings.half_size, settings);
    let offset = gs_base_offset + actual_inclusive - actual;
    let ray_count = min(actual, max(ray_list_limit, offset) - offset);

    ray_count_image[half_pixel] = ray_count;
    pixel_ray_alloc_image[half_pixel] = offset;

    let packed_xy = half_pixel.x | (half_pixel.y << 16u);
    for (u32 sample_index = 0u; sample_index < ray_count; sample_index++) {
        RTGIRayEntry entry;
        entry.packed_xy = packed_xy;
        entry.sample_index = sample_index;
        ray_list[offset + sample_index] = entry;
    }
}
