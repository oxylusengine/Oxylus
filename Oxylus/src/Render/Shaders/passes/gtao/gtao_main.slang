import common;
import gpu;

import scene;

import gtao;

struct ShaderParameters {
  Camera camera_buffer;
  Texture2D<f32> srcWorkingDepth;   // viewspace depth with MIPs, output by XeGTAO_PrefilterDepths16x16 and consumed by XeGTAO_MainPass
  Texture2D<float4> srcNormalmap;   // source normal map (if used) / default: uint
  RWTexture2D<uint> outWorkingAOTerm;   // output AO term (includes bent normals if enabled - packed as R11G11B10 scaled by AO)
  RWTexture2D<unorm f32> outWorkingEdges;   // output depth-based edges used by the denoiser
  RWTexture2D<f32x3> outDebugNormals;
  SamplerState sampler_point_clamp;
};

// Engine-specific normal map loader
f32x3 LoadNormal( int2 pos, Texture2D<float4> normal_map, Camera cam)
{
#if 0
    // special decoding for external normals stored in 11_11_10 unorm - modify appropriately to support your own encoding 
    uint packedInput = normal_map.Load( int3(pos, 0) ).x;
    float3 unpackedOutput = XeGTAO_R11G11B10_UNORM_to_FLOAT3( packedInput );
    float3 normal = normalize(unpackedOutput * 2.0.xxx - 1.0.xxx);
    #endif
#if 0
    // example of a different encoding
    float3 encodedNormal = normal_map.Load(int3(pos, 0)).xyz;
    float3 normal = normalize(encodedNormal * 2.0.xxx - 1.0.xxx);
#endif
    const f32x4 mapped_smooth_normal = normal_map.Load(int3(pos, 0)).xyzw;
    const f32x3 mapped_normal = com::oct_to_vec3(mapped_smooth_normal.xy);
    f32x3 smooth_normal = com::oct_to_vec3(mapped_smooth_normal.zw);

    smooth_normal = mul(cam.view, f32x4(smooth_normal, 1)).xyz;

    return normalize(smooth_normal);
}

// Engine-specific screen & temporal noise loader
f32x2 SpatioTemporalNoise( uint2 pixCoord, uint temporalIndex )    // without TAA, temporalIndex is always 0
{
    float2 noise;
#if 1   // Hilbert curve driving R2 (see https://www.shadertoy.com/view/3tB3z3)
    #ifdef XE_GTAO_HILBERT_LUT_AVAILABLE // load from lookup texture...
        uint index = g_srcHilbertLUT.Load( uint3( pixCoord % 64, 0 ) ).x;
    #else // ...or generate in-place?
        uint index = HilbertIndex( pixCoord.x, pixCoord.y );
    #endif
    index += 288*(temporalIndex%64); // why 288? tried out a few and that's the best so far (with XE_HILBERT_LEVEL 6U) - but there's probably better :)
    // R2 sequence - see http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
    return f32x2( frac( 0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114) ) );
#else   // Pseudo-random (fastest but looks bad - not a good choice)
    uint baseHash = Hash32( pixCoord.x + (pixCoord.y << 15) );
    baseHash = Hash32Combine( baseHash, temporalIndex );
    return lpfloat2( Hash32ToFloat( baseHash ), Hash32ToFloat( Hash32( baseHash ) ) );
#endif
}

[numthreads(8, 8, 1)]
void cs_main(const uint2 pixCoord : SV_DispatchThreadID, 
            uniform ParameterBlock<ShaderParameters> params,
            uniform GTAOConstants gtao_consts,
            uniform u32 quality_level)
{
  switch(quality_level) {
    case 0: {
      XeGTAO_MainPass(pixCoord, 1, 2, SpatioTemporalNoise(pixCoord, gtao_consts.NoiseIndex), LoadNormal(pixCoord, params.srcNormalmap, params.camera_buffer), gtao_consts, params.srcWorkingDepth, params.sampler_point_clamp, params.outWorkingAOTerm, params.outWorkingEdges, params.outDebugNormals );
      break;
    }
    case 1: {
      XeGTAO_MainPass(pixCoord, 2, 2, SpatioTemporalNoise(pixCoord, gtao_consts.NoiseIndex), LoadNormal(pixCoord, params.srcNormalmap, params.camera_buffer), gtao_consts, params.srcWorkingDepth, params.sampler_point_clamp, params.outWorkingAOTerm, params.outWorkingEdges, params.outDebugNormals );
      break;
    }
    case 2: {
      XeGTAO_MainPass(pixCoord, 3, 3, SpatioTemporalNoise(pixCoord, gtao_consts.NoiseIndex), LoadNormal(pixCoord, params.srcNormalmap, params.camera_buffer), gtao_consts, params.srcWorkingDepth, params.sampler_point_clamp, params.outWorkingAOTerm, params.outWorkingEdges, params.outDebugNormals );
      break;
    }
    case 3: {
      XeGTAO_MainPass(pixCoord, 9, 3, SpatioTemporalNoise(pixCoord, gtao_consts.NoiseIndex), LoadNormal(pixCoord, params.srcNormalmap, params.camera_buffer), gtao_consts, params.srcWorkingDepth, params.sampler_point_clamp, params.outWorkingAOTerm, params.outWorkingEdges, params.outDebugNormals );
      break;
    }
  }
}