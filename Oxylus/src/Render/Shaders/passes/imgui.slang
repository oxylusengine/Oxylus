module imgui;

import common;
import gpu;

[[vk::binding(0, 0)]]
Sampler sampler;
[[vk::binding(1, 0)]]
Image2D<f32x4> texture;

struct VertexInput {
    f32x2 position  : POSITION;
    f32x2 tex_coord : TEXCOORD;
    f32x4 color     : COLOR;
};

struct VertexOutput {
    f32x4 position     : SV_Position;
    f32x2 tex_coord    : TEXCOORD;
    f32x4 color        : COLOR;
};

struct PushConstants {
    f32x2 translate;
    f32x2 scale;
};
[[vk::push_constant]] PushConstants c;

[[shader("vertex")]]
func vs_main(VertexInput input) -> VertexOutput {
    VertexOutput output;

    output.position = f32x4(input.position * c.scale + c.translate, 0.0, 1.0);
    output.tex_coord = input.tex_coord;
    output.color.rgb = com::srgb_to_linear(input.color.rgb);
    output.color.a = input.color.a;

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_TARGET {
    return texture.sample(sampler, input.tex_coord) * input.color;
}

