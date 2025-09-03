///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation 
// 
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion", 
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
// 
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>         (\_/)
// Version:         (see XeGTAO.h)                                                                            (='.'=)
// Details:         https://github.com/GameTechDev/XeGTAO                                                     (")_(")
//
// Version history: see XeGTAO.h
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

import common;
import gpu;

import gtao;

struct ShaderParameters {
  Texture2D<uint> srcWorkingAOTerm; // coming from previous pass
  Texture2D<f32> srcWorkingEdges;  // coming from previous pass
  RWTexture2D<uint> outFinalAOTerm; // final AO term - just 'visibility' or 'visibility + bent normals'
  SamplerState sampler_point_clamp;
};

[numthreads(8, 8, 1)]
void cs_main(const uint2 dispatchThreadID : SV_DispatchThreadID, 
                    uniform ParameterBlock<ShaderParameters> params,
                    uniform GTAOConstants gtao_consts,
                    uniform bool last_pass) {
    const uint2 pixCoordBase = dispatchThreadID * uint2( 2, 1 );    // we're computing 2 horizontal pixels at a time (performance optimization)
    // g_samplerPointClamp is a sampler with D3D12_FILTER_MIN_MAG_MIP_POINT filter and D3D12_TEXTURE_ADDRESS_MODE_CLAMP addressing mode
    XeGTAO_Denoise( pixCoordBase, gtao_consts, params.srcWorkingAOTerm, params.srcWorkingEdges, params.sampler_point_clamp, params.outFinalAOTerm, last_pass );
}