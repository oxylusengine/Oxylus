import common;
import gpu;
import scene;

struct ShaderParameters {
    Image2D<f32> heightmap;
    SamplerState heightmap_sampler;
    RWStructuredBuffer<TerrainBrushHit> hit;
    RWStructuredBuffer<TerrainRegion> region;
};

static constexpr u32 MARCH_STEPS = 512;
static constexpr u32 REFINE_STEPS = 24;

func sample_height(in ShaderParameters params, in TerrainBrushParams brush, f32x2 world_xz) -> f32 {
    let uv = brush.world_to_uv(world_xz);
    return brush.decode_height(params.heightmap.SampleLevel(params.heightmap_sampler, uv, 0.0));
}

func clip_to_footprint(in TerrainBrushParams brush, f32x3 origin, f32x3 direction) -> f32x2 {
    let world_max = brush.world_min + brush.world_size;

    var near = 0.0;
    var far = 1e9;

    for (var axis = 0; axis < 2; axis++) {
        let o = axis == 0 ? origin.x : origin.z;
        let d = axis == 0 ? direction.x : direction.z;
        let lo = axis == 0 ? brush.world_min.x : brush.world_min.y;
        let hi = axis == 0 ? world_max.x : world_max.y;

        if (abs(d) < 1e-6) {
            if (o < lo || o > hi) {
                return f32x2(1.0, 0.0);
            }
            continue;
        }

        let t0 = (lo - o) / d;
        let t1 = (hi - o) / d;
        near = max(near, min(t0, t1));
        far = min(far, max(t0, t1));
    }

    return f32x2(near, far);
}

[[shader("compute")]]
[[numthreads(1, 1, 1)]]
func cs_main(uniform ParameterBlock<ShaderParameters> params, uniform TerrainBrushParams brush) -> void {
    let direction = normalize(brush.ray_direction);
    let span = clip_to_footprint(brush, brush.ray_origin, direction);

    TerrainBrushHit result = {};

    if (span.x <= span.y) {
        let step = (span.y - span.x) / f32(MARCH_STEPS);

        var previous_t = span.x;
        var previous_gap = brush.ray_origin.y + direction.y * previous_t -
                           sample_height(params, brush, brush.ray_origin.xz + direction.xz * previous_t);

        for (var i = 1u; i <= MARCH_STEPS; i++) {
            let t = span.x + step * f32(i);
            let position = brush.ray_origin + direction * t;
            let gap = position.y - sample_height(params, brush, position.xz);

            if (gap <= 0.0 && previous_gap > 0.0) {
                var lo = previous_t;
                var hi = t;
                for (var j = 0u; j < REFINE_STEPS; j++) {
                    let mid = (lo + hi) * 0.5;
                    let mid_position = brush.ray_origin + direction * mid;
                    if (mid_position.y - sample_height(params, brush, mid_position.xz) > 0.0) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }

                result.world_position = brush.ray_origin + direction * hi;
                result.valid = 1u;
                break;
            }

            previous_t = t;
            previous_gap = gap;
        }
    }

    params.hit[0] = result;

    TerrainRegion region = {};
    if (result.valid != 0u) {
        let center_uv = brush.world_to_uv(result.world_position.xz);
        let center_texel = center_uv * f32x2(brush.resolution);
        let radius = brush.radius_texels + 1.0;

        region.texel_origin = u32x2(max(floor(center_texel - radius), f32x2(0.0)));

        let patch_scale = f32x2(brush.patch_count) / f32x2(brush.resolution);
        region.patch_origin = u32x2(max(floor((center_texel - radius) * patch_scale), f32x2(0.0)));
    }

    params.region[0] = region;
}
