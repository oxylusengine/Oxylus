#include "../Globals.hlsli"

struct VOut {
  float4 position : SV_POSITION;
  float4 color : COLOR;
};

VOut VSmain(Vertex input) {
  VOut vout;

  vout.position = mul(get_camera(0).projection_view, float4(input.position.unpack(), 1.0));
  vout.color = float4(oct_to_vec3(unpack_snorm2_x16(input.normal)), 1.0f);

  return vout;
}

float4 PSmain(VOut vin) : SV_TARGET0 { return vin.color; }
