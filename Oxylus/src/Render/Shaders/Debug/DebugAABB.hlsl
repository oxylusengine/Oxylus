#include "../Globals.hlsli"

struct VOut {
  float4 position : SV_POSITION;
  float4 color : COLOR;
};

VOut VSmain(uint vertex_index : SV_VertexID, uint instance_id : SV_InstanceID) {
  VOut vout;

  DebugAabb box = get_debug_aabb(instance_id);
  vout.color = box.color.unpack();

  float3 a_pos = create_cube(vertex_index) - 0.5;
  float3 worldPos = a_pos * box.extent.unpack() + box.center.unpack();
  vout.position = mul(get_camera(0).projection_view, float4(worldPos, 1.0));

  return vout;
}

// TODO: Output reactive mask for this
float4 PSmain(VOut vin) : SV_TARGET0 { return vin.color; }
