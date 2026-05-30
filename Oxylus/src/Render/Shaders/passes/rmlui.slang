module rmlui;

import common;
import gpu;

[[vk::binding(0, 0)]]
Sampler sampler;
[[vk::binding(1, 0)]]
Image2D<f32x4> texture;

struct VertexInput {
    f32x2 position  : POSITION;
    f32x4 color     : COLOR;
    f32x2 tex_coord : TEXCOORD;
};

struct VertexOutput {
    f32x4 position     : SV_Position;
    f32x4 color        : COLOR;
    f32x2 tex_coord    : TEXCOORD;
};

struct PushConstants {
    f32x2 translate;
    f32x2 screen_size;
};
[[vk::push_constant]] PushConstants c;

[[shader("vertex")]]
func vs_main(VertexInput input) -> VertexOutput {
    VertexOutput output;

    f32x2 pixel_pos = input.position + c.translate;
    f32x2 normalized = pixel_pos / c.screen_size;
    f32x2 clip_space = (normalized * 2.0) - 1.0;
    output.position = f32x4(clip_space, 0.0, 1.0);
    output.tex_coord = input.tex_coord;
    output.color = input.color;

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_TARGET {
    return texture.Sample(sampler, input.tex_coord) * input.color;
}
