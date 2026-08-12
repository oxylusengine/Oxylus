module terrain_generate;

import common;
import gpu;
import scene;
import terrain;

struct ShaderParameters {
    StorageImage2D<f32, ImageFormat::R16> heightmap;
    StorageImage2D<f32, ImageFormat::R16F> ridgemap;
};

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id: SV_DispatchThreadID, uniform ParameterBlock<ShaderParameters> params,
             uniform TerrainGenerate settings) -> void {
    if (any(thread_id.xy >= settings.resolution)) {
        return;
    }

    let uv = (f32x2(thread_id.xy) + 0.5) / f32x2(settings.resolution);
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
    let height = base.x + erosion.height_slope_delta.x + offset;

    params.heightmap.Store(thread_id.xy, saturate(height));
    params.ridgemap.Store(thread_id.xy, erosion.ridge_map);
}
