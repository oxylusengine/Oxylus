import common;
import gpu;
import sky;
import scene;

struct PushConstants {
    Camera* camera;
};
[[vk::push_constant]] PushConstants C;

struct VertexOutput {
    f32x4 position : SV_Position;
    f32x2 tex_coord : TEXCOORD;
};

struct VertexInput {
  f32x3 position;
};

[[shader("vertex")]]
func vs_main(VertexInput input) -> VertexOutput{
    VertexOutput output;
    //output.tex_coord = input.tex_coord;
    output.position = mul(C.camera.projection_view, f32x4(input.position, 1.0));

    return output;
}

[[shader("fragment")]]
f32x4 fs_main(VertexOutput input) : SV_TARGET {
  return f32x4(0.5, 0.5, 0.5, 1);
}