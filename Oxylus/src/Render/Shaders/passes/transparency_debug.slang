import common;
import gpu;
import scene;
import visbuffer;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> revealage_image;
[[vk::binding(2, 0)]] Texture2D<f32> revealage_front_image;
[[vk::binding(3, 0)]] Texture2D<u32> refractor_visbuffer;
[[vk::binding(4, 0)]] Texture2D<f32> refractor_depth;
[[vk::binding(5, 0)]] Texture2D<f32> reactive_mask;
[[vk::binding(6, 0)]] StructuredBuffer<MeshletInstance> transparent_meshlet_instances;

func hash(u32 a) -> u32 {
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}

// the debug target is display sized while these are render sized, or 1x1 stand-ins when the frame lacks them
func load_scaled<T : ITexelElement>(Texture2D<T> image, f32x2 uv) -> T {
    var width = 0u;
    var height = 0u;
    var levels = 0u;
    image.GetDimensions(0, width, height, levels);
    let pixel = min(u32x2(uv * f32x2(width, height)), u32x2(width, height) - 1);
    return image.Load(i32x3(i32x2(pixel), 0));
}

[[shader("fragment")]]
func fs_main(VertexOutput input, uniform DebugView debug_view) -> f32x4 {
    let uv = input.tex_coord;
    switch (debug_view) {
        case DebugView::TransparencyCoverage: {
            // red behind the refractors or everything when there are none, green in front of them
            let rear = 1.0 - load_scaled(revealage_image, uv);
            let front = 1.0 - load_scaled(revealage_front_image, uv);
            return f32x4(rear, front, 0.0, 1.0);
        }
        case DebugView::Refractors: {
            let texel = load_scaled(refractor_visbuffer, uv);
            if (texel == ~0u) {
                return f32x4(0.0, 0.0, 0.0, 1.0);
            }

            let meshlet_instance = transparent_meshlet_instances[VisBufferData(texel).meshlet_instance_index];
            let h = hash(meshlet_instance.mesh_instance_index);
            return f32x4(f32x3(f32(h & 255), f32((h >> 8) & 255), f32((h >> 16) & 255)) / 255.0, 1.0);
        }
        case DebugView::RefractorDepth: {
            let is_refractor = load_scaled(refractor_visbuffer, uv) != ~0u;
            // reversed infinite depth, the view distance is near over depth
            let view_distance = camera.near_clip / max(load_scaled(refractor_depth, uv), 1e-7);
            let shade = exp(-view_distance / 25.0);
            return f32x4(select(is_refractor, shade, 0.0).xxx, 1.0);
        }
        default: {
            return f32x4(load_scaled(reactive_mask, uv).xxx, 1.0);
        }
    }
}
