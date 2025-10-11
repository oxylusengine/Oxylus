module grid;

import common;
import scene;
import gpu;

#define EPSILON 0.00001

struct PushConstants {
  Camera* camera_buffer;
  f32x4x4 grid_transform;
  f32 grid_scale;
};
[[vk::push_constant]] PushConstants C;

struct ShaderParameters {
  Image2D<f32> depth_image;
  Sampler depth_sampler;
};

struct Vertex {
  f32x3 position : Position;
  f32x2 uv : TEXCOORD0;
};

struct VOutput {
  f32x4 Position : SV_Position;
  f32x2 uv : TEXCOORD;
  f32x2 world_uv : TEXCOORD;
};

float PristineGrid(float2 uv, float2 lineWidth) {
    lineWidth = saturate(lineWidth);
    float4 uvDDXY = float4(ddx(uv), ddy(uv));
    float2 uvDeriv = float2(length(uvDDXY.xz), length(uvDDXY.yw));
    bool2 invertLine = lineWidth > 0.5;
    float2 targetWidth = select(invertLine, 1.0 - lineWidth, lineWidth);
    float2 drawWidth = clamp(targetWidth, uvDeriv, 0.5);
    float2 lineAA = max(uvDeriv, 0.000001) * 1.5;
    float2 gridUV = abs(frac(uv) * 2.0 - 1.0);
    gridUV = select(invertLine, gridUV, 1.0 - gridUV);
    float2 grid2 = smoothstep(drawWidth + lineAA, drawWidth - lineAA, gridUV);
    grid2 *= saturate(targetWidth / drawWidth);
    grid2 = lerp(grid2, targetWidth, saturate(uvDeriv * 2.0 - 1.0));
    grid2 = select(invertLine, 1.0 - grid2, grid2);
    return lerp(grid2.x, 1.0, grid2.y);
}

[[shader("vertex")]]
VOutput vs_main(Vertex inVertex) {
  f32x4x4 mvp = mul(C.camera_buffer.projection_view, C.grid_transform);
  VOutput output;
  output.Position = mul(mvp, f32x4(inVertex.position, 1.0));

  f32 grid_scale = C.grid_scale;
  output.world_uv = inVertex.uv;
  output.uv = inVertex.uv * grid_scale;

  return output;
}

[[shader("fragment")]]
f32x4 fs_main(VOutput input, f32x4 pixel_position : SV_Position) {
  f32 line_width_x = 0.01f;
  f32 line_width_y = 0.01f;
  float grid = PristineGrid(input.uv, float2(line_width_x, line_width_y));

  f32x3 color = f32x3(grid, grid, grid);

  f32 axis_width = 0.01f / C.grid_scale;

  f32 distToXAxis = abs(input.world_uv.y - 0.5);
  if (distToXAxis < axis_width) {
    color = f32x3(1.0, 0.0, 0.0); // Red for X axis
  }
  f32 distToZAxis = abs(input.world_uv.x - 0.5);
  if (distToZAxis < axis_width) {
    color = f32x3(0.0, 0.0, 1.0); // Blue for Z axis
  }

  return f32x4(color, grid);
}