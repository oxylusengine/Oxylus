import common;
import gpu;
import scene;
import fog;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] ConstantBuffer<VolumetricFogParams> params;
[[vk::binding(2, 0)]] Texture3D<f32x4> froxels;
[[vk::binding(3, 0)]] RWTexture3D<f32x4> integrated;

// marches each column front to back, so texel z holds the light and transmittance out to the far edge of slice z
[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x2 id : SV_DispatchThreadID) -> void {
    if (any(id >= params.grid_size.xy)) {
        return;
    }

    let fog = camera.fog;
    let camera_position = camera.position.xyz;
    let screen_uv = (f32x2(id) + 0.5) / f32x2(params.grid_size.xy) / froxel_uv_scale(camera.resolution);
    let ray = normalize(camera.unproject_uv(screen_uv, 0.5) - camera_position);
    // the slices are spaced in view depth, a ray off the screen's center crosses each over a longer path
    let path_scale = 1.0 / max(dot(ray, camera.forward), 1e-4);

    var accumulated = f32x3(0.0);
    var transmittance = 1.0;
    var previous_depth = 0.0;
    for (u32 z = 0; z < params.grid_size.z; z++) {
        let depth = froxel_view_depth(fog, f32(z + 1) / f32(params.grid_size.z));
        let thickness = (depth - previous_depth) * path_scale;
        previous_depth = depth;

        let froxel = froxels.Load(i32x4(i32x2(id), i32(z), 0));
        let extinction = max(froxel.a, 1e-6);
        let slice_transmittance = exp(-extinction * thickness);
        // integrated across the slice instead of taken at a point, so the result holds as the slice count changes
        accumulated += transmittance * (froxel.rgb - froxel.rgb * slice_transmittance) / extinction;
        transmittance *= slice_transmittance;

        integrated.Store(u32x3(id, z), f32x4(accumulated, transmittance));
    }

    // carried through for the analytic fog beyond the froxels
    let far_texel = u32x3(id, FROXEL_DEPTH_SLICES);
    integrated.Store(far_texel, froxels.Load(i32x4(i32x3(far_texel), 0)));
}
