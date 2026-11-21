import common;
import gpu;
import contact_shadow;

[[vk::binding(0)]] Texture2D<f32> scene_depth;
[[vk::binding(1)]] RWTexture2D<f32, 1, ImageFormat.R8> visibility;

// neighboring receivers march along nearly parallel lines toward the projected sun
// cache their common strip of depth, sheared in the minor axis to follow those lines
static const int TILE = 8;
static const int STRIP_WIDTH = 2 * TILE;
static const int MARGIN = TILE / 2;
static const f32 TILE_CENTER = (TILE - 1) * 0.5;
groupshared f32 strip[(TILE + CONTACT_SHADOW_MAX_STEPS) * STRIP_WIDTH];
groupshared u32 trace_steps;
groupshared u32 nonplanar;
groupshared f32x3 tile_plane;

struct DepthStrip : IContactDepth {
    int2 origin;
    bool transpose;
    bool reverse;
    f32 slope;

    int minor_origin(int column) {
        return int(round((f32(column) - TILE_CENTER) * slope)) - MARGIN;
    }

    int2 coord(int column, int row) {
        let local = int2(reverse ? TILE - 1 - column : column, row + minor_origin(column));
        return origin + (transpose ? local.yx : local);
    }

    f32 load(int2 pixel, int2 extent) {
        var local = pixel - origin;
        if (transpose) local = local.yx;
        let column = reverse ? TILE - 1 - local.x : local.x;
        let row = local.y - minor_origin(column);
        if (uint(column) < TILE + trace_steps && uint(row) < STRIP_WIDTH) {
            return strip[column * STRIP_WIDTH + (local.y & (STRIP_WIDTH - 1))];
        }
        // neighbors of a receiver on the edge of the strip may lie outside it
        return all(pixel >= 0) && all(pixel < extent) ? scene_depth.Load(int3(pixel, 0)) : 0.0;
    }
};

struct SunContactLine : IContactLine {
    ScreenContactLine screen;
    bool cached;
    int column;
    int start_row;

    f32x2 load_pair(uint step, int row) {
        if (cached) {
            let index = (column + step) * STRIP_WIDTH;
            return f32x2(strip[index + (row & (STRIP_WIDTH - 1))], strip[index + ((row + 1) & (STRIP_WIDTH - 1))]);
        }
        return screen.load_pair(step, row - start_row);
    }
};

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(uint3 thread : SV_DispatchThreadID, uint lane : SV_GroupIndex, uniform ContactShadowSettings settings) {
    uint width, height;
    scene_depth.GetDimensions(width, height);
    let extent = int2(width, height);
    let pixel = int2(thread.xy);
    let in_view = all(pixel < extent);
    let depth = in_view ? scene_depth.Load(int3(pixel, 0)) : 0.0;
    let light = settings.light_clip;
    let ray = project_contact_ray(pixel, extent, depth, light, settings.trace.length_ws, settings.trace);
    let steps = ray.steps;

    let origin = (pixel / TILE) * TILE;
    if (lane == 0) {
        trace_steps = 0;
        nonplanar = 0;
        let center = min(origin + TILE / 2, extent - 1);
        let center_depth = scene_depth.Load(int3(center, 0));
        tile_plane = f32x3(
            center_depth - scene_depth.Load(int3(max(center - int2(1, 0), 0), 0)),
            center_depth - scene_depth.Load(int3(max(center - int2(0, 1), 0), 0)),
            center_depth);
    }
    GroupMemoryBarrierWithGroupSync();
    InterlockedMax(trace_steps, steps);
    GroupMemoryBarrierWithGroupSync();
    if (trace_steps == 0) {
        if (in_view) visibility[pixel] = 1.0;
        return;
    }

    DepthStrip cache;
    cache.origin = origin;
    let center_delta = (0.5 * f32x2(extent)) * (light.xy + light.w) - (f32x2(cache.origin) + TILE * 0.5) * light.w;
    cache.transpose = abs(center_delta.y) > abs(center_delta.x);
    let axes = cache.transpose ? center_delta.yx : center_delta;
    cache.reverse = axes.x < 0.0;
    cache.slope = axes.y / max(abs(axes.x), 1e-6);
    // a planar strip cannot cast a shadow on itself. this also skips empty sky taps
    let plane_origin = min(origin + TILE / 2, extent - 1);
    bool off_plane = false;
    for (uint i = lane; i < (TILE + trace_steps) * STRIP_WIDTH; i += TILE * TILE) {
        let p = cache.coord(i / STRIP_WIDTH, i % STRIP_WIDTH);
        let sample = all(p >= 0) && all(p < extent) ? scene_depth.Load(int3(p, 0)) : 0.0;
        let minor = cache.transpose ? p.x - origin.x : p.y - origin.y;
        strip[(i / STRIP_WIDTH) * STRIP_WIDTH + (minor & (STRIP_WIDTH - 1))] = sample;
        let expected = tile_plane.z + dot(f32x2(p - plane_origin), tile_plane.xy);
        let unpack = settings.trace.depth_unpack;
        let inv_w = sample * unpack.x + unpack.y;
        let plane_epsilon = 0.002 * settings.trace.thickness * inv_w * inv_w / unpack.z;
        off_plane = off_plane || (sample > 0.0 && abs(sample - expected) > plane_epsilon);
    }
    if (off_plane) InterlockedOr(nonplanar, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (!in_view) return;
    if (steps == 0u) {
        visibility[pixel] = 1.0;
        return;
    }

    let vertical = ray.vertical;
    let local = vertical ? (pixel - origin).yx : pixel - origin;
    let minor_step = vertical ? ray.pixel_step.x : ray.pixel_step.y;
    let major_sign = ray.major_sign;
    let column = major_sign < 0 ? TILE - 1 - local.x : local.x;

    // the distance from the sheared center line varies linearly. if both ends
    // fit, every pair of minor-axis taps is in the strip, including rounding
    let first_row = local.y - (column - TILE_CENTER) * cache.slope;
    let last_row = first_row + (minor_step - cache.slope) * steps;
    let cached = vertical == cache.transpose && (major_sign < 0) == cache.reverse &&
                 min(first_row, last_row) >= 1 - MARGIN && max(first_row, last_row) <= STRIP_WIDTH - MARGIN - 2;
    if (cached && nonplanar == 0) {
        visibility[pixel] = 1.0;
        return;
    }
    let slope = f32x2(
        receiver_slope(cache, pixel, int2(1, 0), extent, depth),
        receiver_slope(cache, pixel, int2(0, 1), extent, depth));
    let screen = ScreenContactLine({scene_depth}, pixel, extent, vertical, major_sign);
    let line = SunContactLine(screen, cached, column, local.y);
    visibility[pixel] = trace_contact_ray(line, ray, local.y, depth, slope, settings.trace);
}
