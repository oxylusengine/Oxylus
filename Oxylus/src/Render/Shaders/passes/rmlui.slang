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
    // projection * element transform, in RmlUi pixels
    f32x4x4 transform;
    // the compiled geometry, drawn without an index buffer by reading its indices here
    RmlVertex* vertices;
    u32* indices;
    // null unless the draw is a gradient, whose tex coords are then in pixels
    RmlGradient* gradient;
    f32x2 translate;
    u32 image_index;
    u32 sampler_index;
    u32 is_texture_srgb;
    // generated textures and saved layers, an image file is straight
    u32 is_texture_premultiplied;
};
[[vk::push_constant]] PushConstants c;

// GLSL's mod, the result takes the divisor's sign
func floor_mod(f32 x, f32 y) -> f32 {
    return x - y * floor(x / y);
}

func gradient_color(in RmlGradient gradient, f32x2 position) -> f32x4 {
    var t = 0.0;
    switch (gradient.function) {
        case RmlGradientFunction.Linear:
        case RmlGradientFunction.RepeatingLinear: {
            t = dot(gradient.v, position - gradient.p) / dot(gradient.v, gradient.v);
        } break;
        case RmlGradientFunction.Radial:
        case RmlGradientFunction.RepeatingRadial: {
            t = length(gradient.v * (position - gradient.p));
        } break;
        default: {
            let offset = position - gradient.p;
            let rotated = f32x2(
                gradient.v.x * offset.x + gradient.v.y * offset.y,
                -gradient.v.y * offset.x + gradient.v.x * offset.y
            );
            t = 0.5 + atan2(-rotated.x, rotated.y) / TAU;
        } break;
    }

    let is_repeating = gradient.function == RmlGradientFunction.RepeatingLinear ||
        gradient.function == RmlGradientFunction.RepeatingRadial ||
        gradient.function == RmlGradientFunction.RepeatingConic;
    let t0 = gradient.stop_positions[0];
    let t1 = gradient.stop_positions[gradient.stop_count - 1];
    if (is_repeating && t1 > t0) {
        t = t0 + floor_mod(t - t0, t1 - t0);
    }

    var color = gradient.stop_colors[0];
    for (u32 i = 1; i < gradient.stop_count; i++) {
        let blend = smoothstep(gradient.stop_positions[i - 1], gradient.stop_positions[i], t);
        color = lerp(color, gradient.stop_colors[i], blend);
    }

    return color;
}

[[shader("vertex")]]
func vs_main(u32 vertex_id : SV_VertexID) -> VertexOutput {
    let input = c.vertices[c.indices[vertex_id]];
    let color = f32x4(
        f32(input.color & 0xFF),
        f32((input.color >> 8) & 0xFF),
        f32((input.color >> 16) & 0xFF),
        f32((input.color >> 24) & 0xFF)
    ) / 255.0;

    VertexOutput output;
    // w is left for the rasterizer, a perspective transform depends on the divide
    output.position = mul(c.transform, f32x4(input.position + c.translate, 0.0, 1.0));
    output.tex_coord = input.tex_coord;
    output.color = color;

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_TARGET {
    if (c.gradient != nullptr) {
        return input.color * gradient_color(*c.gradient, input.tex_coord);
    }

    f32x4 tex_color = global_images[NonUniformResourceIndex(c.image_index)]
        .Sample(global_samplers[NonUniformResourceIndex(c.sampler_index)], input.tex_coord);
    if (c.is_texture_premultiplied != 0) {
        return tex_color * input.color;
    }

    if (c.is_texture_srgb != 0) {
        tex_color.rgb = com::linear_to_srgb(tex_color.rgb);
    }

    return f32x4(tex_color.rgb * tex_color.a, tex_color.a) * input.color;
}

// clip mask geometry only touches the stencil
[[shader("fragment")]]
func fs_clip(VertexOutput input) -> void {}
