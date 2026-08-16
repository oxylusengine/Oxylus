import common;
import gpu;
import scene;
import terrain;

struct ShaderParameters {
    Texture2D<f32> heightmap;
    [[format("r32f")]] RWTexture2D<f32> height_edit;
    [[format("rgba8")]] RWTexture2D<f32x4> splat_edit;
    StructuredBuffer<TerrainBrushHit> hit;
};

func brush_falloff(in TerrainBrushParams brush, f32 distance_texels) -> f32 {
    if (brush.radius_texels <= 0.0) {
        return 0.0;
    }

    let t = saturate(1.0 - distance_texels / brush.radius_texels);
    let smoothed = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);

    return pow(smoothed, max(brush.falloff, 1e-3));
}

func load_height(in ShaderParameters params, i32x2 coord, u32x2 resolution) -> f32 {
    let clamped = clamp(coord, i32x2(0), i32x2(resolution) - 1);
    return params.heightmap.Load(i32x3(clamped, 0));
}

func smoothed_height(in ShaderParameters params, u32x2 texel, u32x2 resolution) -> f32 {
    var sum = 0.0;
    var count = 0.0;

    for (var y = -2; y <= 2; y++) {
        for (var x = -2; x <= 2; x++) {
            sum += load_height(params, i32x2(texel) + i32x2(x, y), resolution);
            count += 1.0;
        }
    }

    return sum / count;
}

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id: SV_DispatchThreadID, uniform ParameterBlock<ShaderParameters> params,
             uniform TerrainBrushParams brush) -> void {
    let hit = params.hit[0];
    if (hit.valid == 0u) {
        return;
    }

    let center = brush.world_to_uv(hit.world_position.xz) * f32x2(brush.resolution);
    let radius = i32(ceil(brush.radius_texels));

    let signed_texel = i32x2(floor(center)) - radius + i32x2(thread_id.xy);
    if (any(signed_texel < 0) || any(signed_texel >= i32x2(brush.resolution))) {
        return;
    }

    let texel = u32x2(signed_texel);
    let falloff = brush_falloff(brush, distance(f32x2(texel) + 0.5, center));
    if (falloff <= 0.0) {
        return;
    }

    let amount = falloff * brush.strength;
    let mode = (TerrainBrushMode)brush.mode;

    if (mode == TerrainBrushMode::PaintLayer) {
        let target = f32x4(
            brush.layer == 1u ? 1.0 : 0.0,
            brush.layer == 2u ? 1.0 : 0.0,
            brush.layer == 3u ? 1.0 : 0.0,
            1.0
        );

        params.splat_edit[texel] = lerp(params.splat_edit[texel], target, saturate(amount));
        return;
    }

    let height = load_height(params, i32x2(texel), brush.resolution);

    var delta = 0.0;
    switch (mode) {
        case TerrainBrushMode::Smooth:
            delta = (smoothed_height(params, texel, brush.resolution) - height) * amount;
            break;
        case TerrainBrushMode::Flatten:
            delta = (brush.flatten_height - height) * amount;
            break;
        case TerrainBrushMode::Noise:
            delta = noised(f32x2(texel) * 0.05, 0u).x * amount;
            break;
        default:
            delta = amount;
            break;
    }

    params.height_edit[texel] = clamp(params.height_edit[texel] + delta, -1.0, 1.0);
}
