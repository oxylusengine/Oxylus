module terrain_generate;

import common;
import gpu;
import scene;
import terrain;

struct ShaderParameters {
    [[format("r16")]] RWTexture2D<f32> heightmap;
    [[format("r16f")]] RWTexture2D<f32> ridgemap;
    Texture2D<f32> height_edit;
    StructuredBuffer<TerrainRegion> region;
    StructuredBuffer<TerrainTrack> track;
    StructuredBuffer<TerrainTrackPoint> track_points;
    StructuredBuffer<TerrainStamp> stamps;
};

struct GeneratedHeight {
    f32 height;
    f32 ridge;
};

struct TrackHit {
    f32 distance;
    // metres along the track to the closest point
    f32 along;
    f32 half_width;
    f32 shoulder;
};

func generate_height(f32x2 uv, in TerrainGenerate settings) -> GeneratedHeight {
    let p = uv * settings.domain_size;

    var base = fractal_noise(
        p, settings.height_frequency, settings.height_octaves, settings.height_lacunarity, settings.height_gain,
        settings.erosion.seed
    );
    base *= settings.height_amplitude;

    // The fade target wants -1 at valleys and +1 at peaks.
    let fade_target = clamp(base.x / (settings.height_amplitude * 0.6), -1.0, 1.0);
    base.x = base.x * 0.5 + 0.5;

    let erosion = erosion_filter(p, base, fade_target, settings.erosion);

    let offset = lerp(settings.height_offset.x, -fade_target, settings.height_offset.y) *
                       erosion.height_slope_delta.w;

    GeneratedHeight result;
    result.height = base.x + erosion.height_slope_delta.x + offset;
    result.ridge = erosion.ridge_map;
    return result;
}

func nearest_track_point(f32x2 position, StructuredBuffer<TerrainTrackPoint> points, u32 point_count) -> TrackHit {
    TrackHit hit;
    hit.distance = 1e30;
    hit.along = 0.0;
    hit.half_width = 0.0;
    hit.shoulder = 0.0;

    var start_length = 0.0;
    for (var i = 1u; i < point_count; i++) {
        let a = points[i - 1];
        let b = points[i];
        let segment = b.position - a.position;
        let length_sq = dot(segment, segment);
        let t = length_sq > TERRAIN_EPSILON ? saturate(dot(position - a.position, segment) / length_sq) : 0.0;
        let closest = a.position + segment * t;
        let d = distance(position, closest);
        let length = sqrt(length_sq);
        if (d < hit.distance) {
            hit.distance = d;
            hit.along = start_length + t * length;
            hit.half_width = lerp(a.half_width, b.half_width, t);
            hit.shoulder = lerp(a.shoulder, b.shoulder, t);
        }

        start_length += length;
    }

    return hit;
}

// the point `along` metres down the track, clamped to its ends
func track_point_at(f32 along, StructuredBuffer<TerrainTrackPoint> points, u32 point_count) -> f32x2 {
    var remaining = max(along, 0.0);
    for (var i = 1u; i < point_count; i++) {
        let a = points[i - 1].position;
        let b = points[i].position;
        let length = distance(a, b);
        if (remaining <= length || i == point_count - 1) {
            return length > TERRAIN_EPSILON ? lerp(a, b, saturate(remaining / length)) : a;
        }

        remaining -= length;
    }

    return points[0].position;
}

func segment_distance(f32x2 position, f32x2 start, f32x2 end) -> f32 {
    let segment = end - start;
    let length_sq = dot(segment, segment);
    let t = length_sq > TERRAIN_EPSILON ? saturate(dot(position - start, segment) / length_sq) : 0.0;
    return distance(position, start + segment * t);
}

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id: SV_DispatchThreadID, uniform ParameterBlock<ShaderParameters> params,
             uniform TerrainGenerate settings) -> void {
    let texel = params.region[0].texel_origin + thread_id.xy;
    if (any(texel >= settings.resolution)) {
        return;
    }

    let uv = (f32x2(texel) + 0.5) / f32x2(settings.resolution);
    var generated = generate_height(uv, settings);

    let track = params.track[0];
    if (track.point_count >= 2) {
        let hit = nearest_track_point(uv * track.world_size, params.track_points, track.point_count);
        let reach = hit.half_width + max(hit.shoulder, 1e-3);
        if (hit.distance < reach) {
            // the height along the track is averaged, so the track rides over the terrain's bumps rather than through
            // them, and it's level across since every texel beside the same point gets the same height. The taps walk
            // the track itself rather than one segment's tangent, which stepped the height at every waypoint
            // a triangle filter over enough taps that rough terrain can't leak through between them as a washboard
            var track_height = 0.0;
            var weight_sum = 0.0;
            for (var tap = -8; tap <= 8; tap++) {
                let along = hit.along + f32(tap) * track.smoothing / 8.0;
                let point = track_point_at(along, params.track_points, track.point_count);
                let weight = 9.0 - f32(abs(tap));
                track_height += generate_height(point / track.world_size, settings).height * weight;
                weight_sum += weight;
            }
            track_height /= weight_sum;

            let blend = 1.0 - smoothstep(hit.half_width, reach, hit.distance);
            generated.height = lerp(generated.height, track_height, blend);
            generated.ridge *= 1.0 - blend;
        }
    }

    // stamps are smooth bumps along a capsule, and they add up
    for (var i = 0u; i < track.stamp_count; i++) {
        let stamp = params.stamps[i];
        let x = segment_distance(uv * track.world_size, stamp.start, stamp.end) / stamp.radius;
        if (x < 1.0) {
            generated.height += stamp.height * (0.5 + 0.5 * cos(3.14159265 * x));
        }
    }

    let edit = params.height_edit.Load(i32x3(i32x2(texel), 0));

    params.heightmap.Store(texel, saturate(generated.height + edit));
    params.ridgemap.Store(texel, generated.ridge);
}
