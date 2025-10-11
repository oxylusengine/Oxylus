module grid;

import common;
import scene;
import gpu;

#define EPSILON 0.00001

struct PushConstants {
  f32x4x4 grid_transform;
  f32 grid_scale;
};
[[vk::push_constant]] PushConstants C;

struct ShaderParameters {
  ConstantBuffer<Camera> camera;
};

struct Vertex {
  f32x3 position : Position;
  f32x2 uv : TEXCOORD0;
};

struct VOutput {
  f32x4 Position : SV_Position;
  f32x2 uv : TEXCOORD0;
  f32x2 world_uv : TEXCOORD1;
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
VOutput vs_main(uniform ParameterBlock<ShaderParameters> params, Vertex inVertex) {
  VOutput output;

  f32x4x4 mvp = mul(params.camera.projection_view, C.grid_transform);
  output.Position = mul(mvp, f32x4(inVertex.position, 1.0));

  output.world_uv = inVertex.uv;

  f32 grid_scale = C.grid_scale;
  output.uv = inVertex.uv * grid_scale;

  return output;
}

[[shader("fragment")]]
f32x4 fs_main(VOutput input) {
  f32 line_width_x = 0.01f;
  f32 line_width_y = 0.01f;
  float grid = PristineGrid(input.uv, float2(line_width_x, line_width_y));

  f32x3 color = f32x3(grid, grid, grid);

  f32 axis_width = line_width_x / C.grid_scale;

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