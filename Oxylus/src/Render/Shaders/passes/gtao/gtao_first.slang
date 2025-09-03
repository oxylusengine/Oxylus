import common;
import gpu;

import gtao;

struct ShaderParameters {
  Texture2D<f32>        srcRawDepth;
  RWTexture2D<f32>        outWorkingDepthMIP0;
  RWTexture2D<f32>        outWorkingDepthMIP1;
  RWTexture2D<f32>        outWorkingDepthMIP2;
  RWTexture2D<f32>        outWorkingDepthMIP3;
  RWTexture2D<f32>        outWorkingDepthMIP4;
  SamplerState sampler_point_clamp;
};

// Engine-specific entry point for the first pass
[numthreads(8, 8, 1)]   // <- hard coded to 8x8; each thread computes 2x2 blocks so processing 16x16 block: Dispatch needs to be called with (width + 16-1) / 16, (height + 16-1) / 16
void cs_main(uint2 dispatchThreadID : SV_DispatchThreadID, 
            uint2 groupThreadID : SV_GroupThreadID, 
            uniform ParameterBlock<ShaderParameters> params,
            uniform GTAOConstants gtao_consts) {

    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID, 
                                gtao_consts, params.srcRawDepth, 
                                params.sampler_point_clamp, 
                                params.outWorkingDepthMIP0, 
                                params.outWorkingDepthMIP1, 
                                params.outWorkingDepthMIP2, 
                                params.outWorkingDepthMIP3, 
                                params.outWorkingDepthMIP4);
}