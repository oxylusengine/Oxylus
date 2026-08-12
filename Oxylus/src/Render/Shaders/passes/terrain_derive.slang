module terrain_derive;

import common;
import gpu;
import scene;

struct ShaderParameters {
    Image2D<f32> heightmap;
    Image2D<f32> ridgemap;
    StorageImage2D<f32x2, ImageFormat::RG16F> normalmap;
    StorageImage2D<f32x4, ImageFormat::RGBA8> splatmap;
};

func load_height(in ShaderParameters params, i32x2 coord, u32x2 resolution) -> f32 {
    let clamped = clamp(coord, i32x2(0), i32x2(resolution) - 1);
    return params.heightmap.Load(i32x3(clamped, 0));
}

[[shader("compute")]]
[[numthreads(16, 16, 1)]]
func cs_main(u32x3 thread_id: SV_DispatchThreadID, uniform ParameterBlock<ShaderParameters> params,
             uniform TerrainDerive settings) -> void {
    if (any(thread_id.xy >= settings.resolution)) {
        return;
    }

    let coord = i32x2(thread_id.xy);

    // Sobel over the normalized heightmap, scaled back into world units.
    let h00 = load_height(params, coord + i32x2(-1, -1), settings.resolution);
    let h10 = load_height(params, coord + i32x2(0, -1), settings.resolution);
    let h20 = load_height(params, coord + i32x2(1, -1), settings.resolution);
    let h01 = load_height(params, coord + i32x2(-1, 0), settings.resolution);
    let h21 = load_height(params, coord + i32x2(1, 0), settings.resolution);
    let h02 = load_height(params, coord + i32x2(-1, 1), settings.resolution);
    let h12 = load_height(params, coord + i32x2(0, 1), settings.resolution);
    let h22 = load_height(params, coord + i32x2(1, 1), settings.resolution);

    let dhdx = ((h20 + 2.0 * h21 + h22) - (h00 + 2.0 * h01 + h02)) * settings.height_range /
                     (8.0 * settings.texel_world_size.x);
    let dhdz = ((h02 + 2.0 * h12 + h22) - (h00 + 2.0 * h10 + h20)) * settings.height_range /
                     (8.0 * settings.texel_world_size.y);

    // Y is always positive for a heightfield, so store XZ and reconstruct Y on read.
    let normal = normalize(f32x3(-dhdx, 1.0, -dhdz));
    params.normalmap.Store(thread_id.xy, normal.xz);

    let height = load_height(params, coord, settings.resolution);
    let ridge = params.ridgemap.Load(i32x3(coord, 0));

    // Slope as 0 (flat) .. 1 (vertical).
    let slope = saturate(1.0 - normal.y);
    let rock = smoothstep(settings.slope_rock_begin, settings.slope_rock_end, slope);
    let snow = smoothstep(settings.altitude_snow_begin, settings.altitude_snow_end, height) * (1.0 - rock);
    // Creases (negative ridge) collect sediment and water; that is the drainage layer.
    let drainage = saturate(-ridge * settings.ridge_drainage_scale) * (1.0 - rock) * (1.0 - snow);
    let grass = saturate(1.0 - rock - snow - drainage);

    params.splatmap.Store(thread_id.xy, f32x4(grass, rock, drainage, snow));
}
