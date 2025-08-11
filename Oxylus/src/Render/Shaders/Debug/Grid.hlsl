#include "../Globals.hlsli"

struct VOutput {
  float4 Position : SV_Position;
  float2 UV : TEXCOORD;
  float3 NearPoint : NEAR_POINT;
  float3 FarPoint : FAR_POINT;
};

float3 UnprojectPoint(float x, float y, float z, float4x4 inv_projection_view_matrix) {
  float4 unprojectedPoint = mul(inv_projection_view_matrix, float4(x, y, z, 1.0));
  return unprojectedPoint.xyz / unprojectedPoint.w;
}

VOutput main(Vertex inVertex) {
  VOutput output;
  output.UV = inVertex.uv.unpack();

  float3 position = inVertex.position.unpack();
  output.NearPoint = UnprojectPoint(position.x, position.y, 0.0, get_camera().inv_projection_view);
  output.FarPoint = UnprojectPoint(position.x, position.y, 1.0, get_camera().inv_projection_view).xyz;
  output.Position = float4(position.xyz, 1.0);

  return output;
}

struct PSOut {
  float4 Color : SV_Target;
  float Depth : SV_Depth;
};

static const float subdivisions = 10.0f;

float4 Grid(float3 fragPos3D, float scale) {
  const float2 coord = fragPos3D.xz * scale;

  const float2 derivative = fwidth(coord);
  float2 grid = abs(frac(coord - 0.5) - 0.5) / derivative;
  const float _line = min(grid.x, grid.y);
  const float minimumz = 0.1f;
  const float minimumx = 0.1f;
  float4 color = float4(0.35f, 0.35f, 0.35f, 1.0f - min(_line, 1.0f));
  // z axis
  if (fragPos3D.x > -0.1 * minimumx && fragPos3D.x < 0.1 * minimumx)
    color = float4(0.0, 0.0, 1.0, color.w);
  // x axis
  if (fragPos3D.z > -0.1 * minimumz && fragPos3D.z < 0.1 * minimumz)
    color = float4(1.0, 0.0, 0.0, color.w);
  return color;
}

float ComputeDepth(float3 pos) {
  float4 clipSpacePos = mul(get_camera().projection_view, float4(pos.xyz, 1.0));
  return (clipSpacePos.z / clipSpacePos.w);
}

float ComputeLinearDepth(float3 pos) {
  float4 clipSpacePos = mul(get_camera().projection_view, float4(pos.xyz, 1.0));
  float clipSpaceDepth = (clipSpacePos.z / clipSpacePos.w) * 2.0 - 1.0;                  // put back between -1 and 1
  float linearDepth = (2.0 * get_camera().near_clip * get_camera().far_clip) /
                      (get_camera().far_clip + get_camera().near_clip -
                       clipSpaceDepth * (get_camera().far_clip - get_camera().near_clip)); // get linear value between 0.01 and 100
  return linearDepth / get_camera().far_clip;                                             // normalize
}

int RoundToPowerOfTen(float n) { return int(pow(10.0, floor((1 / log(10)) * log(n)))); }

PSOut PSmain(VOutput input, float4 pixelPosition : SV_Position) {
  PSOut psOut;

  const float t = -input.NearPoint.y / (input.FarPoint.y - input.NearPoint.y);
  if (t < 0.)
    discard;

  float3 pixelPos = input.NearPoint + t * (input.FarPoint - input.NearPoint);
  pixelPos.y -= 0.1; // depth fighting with zero plane fix

  psOut.Depth = ComputeDepth(pixelPos);

  float linearDepth = ComputeLinearDepth(pixelPos);
  float fading = max(0, (0.5 - linearDepth));

  float dist, angleFade;
  if (get_camera().projection[3][3] - EPSILON < 0.0) {
    float3 viewvec = get_camera().position.xyz - pixelPos;
    dist = length(viewvec);
    viewvec /= dist;

    float angle = viewvec.y;
    angle = 1.0 - abs(angle);
    angle *= angle;
    angleFade = 1.0 - angle * angle;
    angleFade *= 1.0 - smoothstep(0.0, get_scene().grid_max_distance, dist - get_scene().grid_max_distance);
  } else {
    dist = pixelPosition.z * 2.0 - 1.0;
    /* Avoid fading in +Z direction in camera view (see T70193). */
    dist = clamp(dist, 0.0, 1.0); // abs(dist);
    angleFade = 1.0 - smoothstep(0.0, 0.5, dist - 0.5);
    dist = 1.0;                   /* avoid branch after */
  }

  const float distanceToCamera = abs(get_camera().position.y - pixelPos.y);
  const int powerOfTen = max(1, RoundToPowerOfTen(distanceToCamera));
  const float divs = 1.0f / float(powerOfTen);
  const float secondFade = smoothstep(subdivisions / divs, 1 / divs, distanceToCamera);

  float4 grid1 = Grid(pixelPos, divs / subdivisions);
  float4 grid2 = Grid(pixelPos, divs);

  grid2.a *= secondFade;
  grid1.a *= (1 - secondFade);

  psOut.Color = grid1 + grid2; // adding multiple resolution for the grid
  psOut.Color.a = max(grid1.a, grid2.a);

  psOut.Color *= float(t > 0);
  psOut.Color.a *= fading;
  psOut.Color.a *= angleFade;

  return psOut;
}
