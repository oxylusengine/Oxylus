import common;
import gpu;
import scene;

struct ShaderParameters {
    Image2D<f32> heightmap;
    StorageImage2D<f32x2, ImageFormat::RG16> patch_minmax;
};

[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id: SV_DispatchThreadID, uniform ParameterBlock<ShaderParameters> params,
             uniform TerrainMinMax settings) -> void {
    if (any(thread_id.xy >= settings.patch_count)) {
        return;
    }

    let scale = f32x2(settings.resolution) / f32x2(settings.patch_count);
    let begin = u32x2(floor(f32x2(thread_id.xy) * scale));
    let end = min(u32x2(ceil(f32x2(thread_id.xy + 1u) * scale)) + 1u, settings.resolution);

    var bounds = f32x2(1.0, 0.0); // (min, max), seeded inverted

    for (var y = begin.y; y < end.y; y++) {
        for (var x = begin.x; x < end.x; x++) {
            let h = params.heightmap.Load(i32x3(i32(x), i32(y), 0));
            bounds.x = min(bounds.x, h);
            bounds.y = max(bounds.y, h);
        }
    }

    params.patch_minmax.Store(thread_id.xy, bounds);
}
