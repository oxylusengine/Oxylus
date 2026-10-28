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
};

struct GeneratedHeight {
    f32 height;
    f32 ridge;
};

struct TrackHit {
    f32 distance;
    f32x2 closest;
    f32x2 tangent;
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
    hit.closest = position;
    hit.tangent = f32x2(1.0, 0.0);
    hit.half_width = 0.0;
    hit.shoulder = 0.0;

    for (var i = 1u; i < point_count; i++) {
        let a = points[i - 1];
        let b = points[i];
        let segment = b.position - a.position;
        let length_sq = dot(segment, segment);
        let t = length_sq > TERRAIN_EPSILON ? saturate(dot(position - a.position, segment) / length_sq) : 0.0;
        let closest = a.position + segment * t;
        let d = distance(position, closest);
        if (d < hit.distance) {
            hit.distance = d;
            hit.closest = closest;
            hit.tangent = length_sq > TERRAIN_EPSILON ? segment * rsqrt(length_sq) : hit.tangent;
            hit.half_width = lerp(a.half_width, b.half_width, t);
            hit.shoulder = lerp(a.shoulder, b.shoulder, t);
        }
    }

    return hit;
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
            // them, and it's level across since every texel beside the same point gets the same height
            var track_height = 0.0;
            for (var tap = -2; tap <= 2; tap++) {
                let along = hit.closest + hit.tangent * (f32(tap) * 0.5 * track.smoothing);
                track_height += generate_height(along / track.world_size, settings).height;
            }
            track_height /= 5.0;

            let blend = 1.0 - smoothstep(hit.half_width, reach, hit.distance);
            generated.height = lerp(generated.height, track_height, blend);
            generated.ridge *= 1.0 - blend;
        }
    }

    let edit = params.height_edit.Load(i32x3(i32x2(texel), 0));

    params.heightmap.Store(texel, saturate(generated.height + edit));
    params.ridgemap.Store(texel, generated.ridge);
}
