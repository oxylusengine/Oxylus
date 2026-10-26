module rmlui;

import common;
import gpu;
import scene;

// Rml::Vertex, the colour is premultiplied rgba8
struct RmlVertex {
    f32x2 position;
    u32 color;
    f32x2 tex_coord;
};

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x4 color;
    f32x2 tex_coord;
};

struct PushConstants {
    f32x4x4 transform;
    // the draw's first vertex, indices are relative to it
    RmlVertex* vertices;
    f32x2 translate;
    f32x2 screen_size;
    u32 image_index;
    u32 sampler_index;
};
[[vk::push_constant]] PushConstants c;

[[shader("vertex")]]
func vs_main(u32 vertex_id : SV_VertexID) -> VertexOutput {
    let input = c.vertices[vertex_id];
    let color = f32x4(
        f32(input.color & 0xFF),
        f32((input.color >> 8) & 0xFF),
        f32((input.color >> 16) & 0xFF),
        f32((input.color >> 24) & 0xFF)
    ) / 255.0;

    VertexOutput output;
    f32x2 pixel_pos = input.position + c.translate;
    f32x4 transformed_pos = mul(c.transform, f32x4(pixel_pos, 0.0, 1.0));
    f32x2 normalized = transformed_pos.xy / c.screen_size;
    f32x2 clip_space = (normalized * 2.0) - 1.0;
    output.position = f32x4(clip_space, 0.0, 1.0);
    output.tex_coord = input.tex_coord;
    output.color = f32x4(pow(color.rgb, f32x3(2.2)), color.a);

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_TARGET {
    f32x4 tex_color = global_images[NonUniformResourceIndex(c.image_index)]
        .Sample(global_samplers[NonUniformResourceIndex(c.sampler_index)], input.tex_coord);
    f32x4 final_color = tex_color * input.color;
    final_color.rgb = final_color.rgb * tex_color.a * input.color.a;

    return final_color;
}
