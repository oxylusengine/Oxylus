import common;
import gpu;
import scene;
import fog;

#include <fullscreen.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Image2D<f32> depth_image;

// blended as `src + dst * src.a`, so rgb is the fog's inscatter and alpha what it lets through
[[shader("fragment")]]
func fs_main(VertexOutput input) -> f32x4 {
    let depth = depth_image.Load(u32x3(u32x2(input.position.xy), 0));
    let camera_position = camera.position.xyz;

    FogSample fog;
    if (depth == 0.0) {
        if ((scene_flags & SceneFlags::TransparentBackground) != 0) {
            discard;
        }

        let direction = normalize(camera.unproject_uv(input.tex_coord, 0.5) - camera_position);
        fog = evaluate_height_fog_sky(camera.fog, camera_position, direction);
    } else {
        fog = evaluate_height_fog(camera.fog, camera_position, camera.unproject_uv(input.tex_coord, depth));
    }

    return f32x4(fog.inscatter, fog.transmittance);
}
