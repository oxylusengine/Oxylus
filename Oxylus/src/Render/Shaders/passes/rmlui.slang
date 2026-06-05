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
    f32x4x4 transform;
};
[[vk::push_constant]] PushConstants c;

[[shader("vertex")]]
func vs_main(VertexInput input) -> VertexOutput {
    VertexOutput output;

    f32x2 pixel_pos = input.position + c.translate;
    f32x4 transformed_pos = mul(c.transform, f32x4(pixel_pos, 0.0, 1.0));
    f32x2 normalized = transformed_pos.xy / c.screen_size;
    f32x2 clip_space = (normalized * 2.0) - 1.0;
    output.position = f32x4(clip_space, 0.0, 1.0);
    output.tex_coord = input.tex_coord;
    output.color = f32x4(pow(input.color.rgb, f32x3(2.2)), input.color.a);

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_TARGET {
    f32x4 tex_color = texture.Sample(sampler, input.tex_coord);
    f32x4 final_color = tex_color * input.color;
    final_color.rgb = final_color.rgb * tex_color.a * input.color.a;

    return final_color;
}
