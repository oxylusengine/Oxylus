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

module gtao;

import common;
import gpu;

#define XE_GTAO_DEPTH_MIP_LEVELS  5                   // this one is hard-coded to 5 for now
#define XE_GTAO_PI                (3.1415926535897932384626433832795)
#define XE_GTAO_PI_HALF           (1.5707963267948966192313216916398)
#define XE_GTAO_OCCLUSION_TERM_SCALE                    (1.5f)      // for packing in UNORM (because raw, pre-denoised occlusion term can overshoot 1 but will later average out to 1)

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
typedef f32x4 AOTermType;            // .xyz is bent normal, .w is visibility term
#else
typedef f32 AOTermType;             // .x is visibility term
#endif

public struct GTAOConstants {
  public i32x2 ViewportSize;
  public f32x2 ViewportPixelSize;                  // .zw == 1.0 / ViewportSize.xy

  public f32x2 DepthUnpackConsts;
  public f32x2 CameraTanHalfFOV;

  public f32x2 NDCToViewMul;
  public f32x2 NDCToViewAdd;

  public f32x2 NDCToViewMul_x_PixelSize;
  public f32 EffectRadius;                       // world (viewspace) maximum size of the shadow
  public f32 EffectFalloffRange;

  public f32 RadiusMultiplier;
  public f32 FinalValuePower;
  public f32 DenoiseBlurBeta;

  public f32 SampleDistributionPower;
  public f32 ThinOccluderCompensation;
  public f32 DepthMIPSamplingOffset;
  public i32 NoiseIndex;                         // frameIndex % 64 if using TAA or 0 otherwise
  public u32 pad_;
};

f32 XeGTAO_DepthMIPFilter( f32 depth0, f32 depth1, f32 depth2, f32 depth3, const GTAOConstants consts )
{
    f32 maxDepth = max( max( depth0, depth1 ), max( depth2, depth3 ) );

    const f32 depthRangeScaleFactor = 0.75; // found empirically :)
    const f32 effectRadius              = depthRangeScaleFactor * (f32)consts.EffectRadius * (f32)consts.RadiusMultiplier;
    const f32 falloffRange              = (f32)consts.EffectFalloffRange * effectRadius;
    const f32 falloffFrom       = effectRadius * ((f32)1-(f32)consts.EffectFalloffRange);
    // fadeout precompute optimisation
    const f32 falloffMul        = (f32)-1.0 / ( falloffRange );
    const f32 falloffAdd        = falloffFrom / ( falloffRange ) + (f32)1.0;

    f32 weight0 = saturate( (maxDepth-depth0) * falloffMul + falloffAdd );
    f32 weight1 = saturate( (maxDepth-depth1) * falloffMul + falloffAdd );
    f32 weight2 = saturate( (maxDepth-depth2) * falloffMul + falloffAdd );
    f32 weight3 = saturate( (maxDepth-depth3) * falloffMul + falloffAdd );

    f32 weightSum = weight0 + weight1 + weight2 + weight3;
    return (weight0 * depth0 + weight1 * depth1 + weight2 * depth2 + weight3 * depth3) / weightSum;
}

float XeGTAO_ScreenSpaceToViewSpaceDepth( const float screenDepth, const GTAOConstants consts )
{
    float depthLinearizeMul = consts.DepthUnpackConsts.x;
    float depthLinearizeAdd = consts.DepthUnpackConsts.y;
    // Optimised version of "-cameraClipNear / (cameraClipFar - projDepth * (cameraClipFar - cameraClipNear)) * cameraClipFar"
    return depthLinearizeMul / (depthLinearizeAdd - screenDepth);
}

// This is also a good place to do non-linear depth conversion for cases where one wants the 'radius' (effectively the threshold between near-field and far-field GI), 
// is required to be non-linear (i.e. very large outdoors environments).
f32 XeGTAO_ClampDepth( f32 depth )
{
    return clamp( depth, 0.0, 3.402823466e+38 );
}


// constexpr f32x4 gather_red(Texture2D<f32> tex, SamplerState samp, f32x2 uv, constexpr i32x2 offset) {
//     f32x4 result;
// 
//     result.x = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,0)).r;
//     result.y = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,0)).r;
//     result.z = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,1)).r;
//     result.w = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,1)).r;
// 
//     return result;
// }

constexpr f32x4 gather_red(Texture2D<f32> tex, SamplerState samp, f32x2 uv, constexpr i32x2 offset) {
    f32x4 result;

    result.z = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,0)).r;
    result.w = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,0)).r;
    result.x = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,1)).r;
    result.y = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,1)).r;

    return result;
}

// constexpr u32x4 gather_red_u(Texture2D<u32> tex, SamplerState samp, f32x2 uv, constexpr i32x2 offset) {
//     u32x4 result;
// 
//     result.x = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,0)).r;
//     result.y = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,0)).r;
//     result.z = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,1)).r;
//     result.w = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,1)).r;
// 
//     return result;
// }

constexpr u32x4 gather_red_u(Texture2D<u32> tex, SamplerState samp, f32x2 uv, constexpr i32x2 offset) {
    u32x4 result;

    result.z = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,0)).r;
    result.w = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,0)).r;
    result.x = tex.SampleLevel(samp, uv, 0, offset + i32x2(0,1)).r;
    result.y = tex.SampleLevel(samp, uv, 0, offset + i32x2(1,1)).r;

    return result;
}

groupshared f32 g_scratchDepths[8][8];
public void XeGTAO_PrefilterDepths16x16(uint2 dispatchThreadID,
                                        uint2 groupThreadID, 
                                        const GTAOConstants consts, 
                                        Texture2D<f32> sourceNDCDepth, 
                                        SamplerState depthSampler, 
                                        RWTexture2D<f32> outDepth0, 
                                        RWTexture2D<f32> outDepth1, 
                                        RWTexture2D<f32> outDepth2, 
                                        RWTexture2D<f32> outDepth3, 
                                        RWTexture2D<f32> outDepth4)
{
    // MIP 0
    const uint2 baseCoord = dispatchThreadID;
    const uint2 pixCoord = baseCoord * 2;
    constexpr i32x2 offset = i32x2(1, 1);
    float4 depths4 = gather_red(sourceNDCDepth, depthSampler, float2( pixCoord * consts.ViewportPixelSize ), offset);
    f32 depth0 = XeGTAO_ClampDepth( XeGTAO_ScreenSpaceToViewSpaceDepth( depths4.w, consts ) );
    f32 depth1 = XeGTAO_ClampDepth( XeGTAO_ScreenSpaceToViewSpaceDepth( depths4.z, consts ) );
    f32 depth2 = XeGTAO_ClampDepth( XeGTAO_ScreenSpaceToViewSpaceDepth( depths4.x, consts ) );
    f32 depth3 = XeGTAO_ClampDepth( XeGTAO_ScreenSpaceToViewSpaceDepth( depths4.y, consts ) );
    outDepth0[ pixCoord + uint2(0, 0) ] = depth0;
    outDepth0[ pixCoord + uint2(1, 0) ] = depth1;
    outDepth0[ pixCoord + uint2(0, 1) ] = depth2;
    outDepth0[ pixCoord + uint2(1, 1) ] = depth3;

    // MIP 1
    f32 dm1 = XeGTAO_DepthMIPFilter( depth0, depth1, depth2, depth3, consts );
    outDepth1[ baseCoord ] = dm1;
    g_scratchDepths[ groupThreadID.x ][ groupThreadID.y ] = dm1;

    GroupMemoryBarrierWithGroupSync( );

    // MIP 2
    if( all( ( groupThreadID.xy % 2.xx ) == 0 ) )
    {
        f32 inTL = g_scratchDepths[groupThreadID.x+0][groupThreadID.y+0];
        f32 inTR = g_scratchDepths[groupThreadID.x+1][groupThreadID.y+0];
        f32 inBL = g_scratchDepths[groupThreadID.x+0][groupThreadID.y+1];
        f32 inBR = g_scratchDepths[groupThreadID.x+1][groupThreadID.y+1];

        f32 dm2 = XeGTAO_DepthMIPFilter( inTL, inTR, inBL, inBR, consts );
        outDepth2[ baseCoord / 2 ] = dm2;
        g_scratchDepths[ groupThreadID.x ][ groupThreadID.y ] = dm2;
    }

    GroupMemoryBarrierWithGroupSync( );

    // MIP 3
    if( all( ( groupThreadID.xy % 4.xx ) == 0 ) )
    {
        f32 inTL = g_scratchDepths[groupThreadID.x+0][groupThreadID.y+0];
        f32 inTR = g_scratchDepths[groupThreadID.x+2][groupThreadID.y+0];
        f32 inBL = g_scratchDepths[groupThreadID.x+0][groupThreadID.y+2];
        f32 inBR = g_scratchDepths[groupThreadID.x+2][groupThreadID.y+2];

        f32 dm3 = XeGTAO_DepthMIPFilter( inTL, inTR, inBL, inBR, consts );
        outDepth3[ baseCoord / 4 ] = dm3;
        g_scratchDepths[ groupThreadID.x ][ groupThreadID.y ] = dm3;
    }

    GroupMemoryBarrierWithGroupSync( );

    // MIP 4
    if( all( ( groupThreadID.xy % 8.xx ) == 0 ) )
    {
        f32 inTL = g_scratchDepths[groupThreadID.x+0][groupThreadID.y+0];
        f32 inTR = g_scratchDepths[groupThreadID.x+4][groupThreadID.y+0];
        f32 inBL = g_scratchDepths[groupThreadID.x+0][groupThreadID.y+4];
        f32 inBR = g_scratchDepths[groupThreadID.x+4][groupThreadID.y+4];

        f32 dm4 = XeGTAO_DepthMIPFilter( inTL, inTR, inBL, inBR, consts );
        outDepth4[ baseCoord / 8 ] = dm4;
        //g_scratchDepths[ groupThreadID.x ][ groupThreadID.y ] = dm4;
    }
}

// From https://www.shadertoy.com/view/3tB3z3 - except we're using R2 here
#define XE_HILBERT_LEVEL    6U
#define XE_HILBERT_WIDTH    ( (1U << XE_HILBERT_LEVEL) )
#define XE_HILBERT_AREA     ( XE_HILBERT_WIDTH * XE_HILBERT_WIDTH )

public uint HilbertIndex(uint posX, uint posY) {
  uint index = 0U;
  for (uint curLevel = XE_HILBERT_WIDTH / 2U; curLevel > 0U; curLevel /= 2U) {
    uint regionX = (posX & curLevel) > 0U;
    uint regionY = (posY & curLevel) > 0U;
    index += curLevel * curLevel * ((3U * regionX) ^ regionY);
    if (regionY == 0U) {
      if (regionX == 1U) {
        posX = uint((XE_HILBERT_WIDTH - 1U)) - posX;
        posY = uint((XE_HILBERT_WIDTH - 1U)) - posY;
      }

      uint temp = posX;
      posX = posY;
      posY = temp;
    }
  }
  return index;
}

f32x4 XeGTAO_CalculateEdges(const f32 centerZ, const f32 leftZ, const f32 rightZ, const f32 topZ, const f32 bottomZ) {
  f32x4 edgesLRTB = f32x4(leftZ, rightZ, topZ, bottomZ) - centerZ;
  f32 slopeLR = (edgesLRTB.y - edgesLRTB.x) * 0.5;
  f32 slopeTB = (edgesLRTB.w - edgesLRTB.z) * 0.5;
  f32x4 edgesLRTBSlopeAdjusted = edgesLRTB + f32x4( slopeLR, -slopeLR, slopeTB, -slopeTB );
  edgesLRTB = min(abs(edgesLRTB), abs(edgesLRTBSlopeAdjusted));
  return f32x4(saturate((1.25 - edgesLRTB / (centerZ * 0.011))));
}

f32 XeGTAO_PackEdges(f32x4 edgesLRTB) {
  // integer version:
  // edgesLRTB = saturate(edgesLRTB) * 2.9.xxxx + 0.5.xxxx;
  // return (((uint)edgesLRTB.x) << 6) + (((uint)edgesLRTB.y) << 4) + (((uint)edgesLRTB.z) << 2) + (((uint)edgesLRTB.w));
  // 
  // optimized, should be same as above
  edgesLRTB = round(saturate(edgesLRTB) * 2.9);
  return dot(edgesLRTB, f32x4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0)) ;
}

f32x3 XeGTAO_ComputeViewspacePosition(const f32x2 screenPos, const float viewspaceDepth, const GTAOConstants consts) {
  f32x3 ret;
  ret.xy = (consts.NDCToViewMul * screenPos.xy + consts.NDCToViewAdd) * viewspaceDepth;
  ret.z = viewspaceDepth;
  return ret;
}

// http://h14s.p5r.org/2012/09/0x5f3759df.html, [Drobot2014a] Low Level Optimizations for GCN, https://blog.selfshadow.com/publications/s2016-shading-course/activision/s2016_pbs_activision_occlusion.pdf slide 63
f32 XeGTAO_FastSqrt(f32 x) {
    return (asfloat( 0x1fbd1df5 + ( asint( x ) >> 1 ) ));
}
// input [-1, 1] and output [0, PI], from https://seblagarde.wordpress.com/2014/12/01/inverse-trigonometric-functions-gpu-optimization-for-amd-gcn-architecture/
f32 XeGTAO_FastACos(f32 inX) { 
  const f32 PI = 3.141593;
  const f32 HALF_PI = 1.570796;
  f32 x = abs(inX); 
  f32 res = -0.156583 * x + HALF_PI; 
  res *= XeGTAO_FastSqrt(1.0 - x); 
  return (inX >= 0) ? res : PI - res; 
}

void XeGTAO_OutputWorkingTerm( const uint2 pixCoord, f32 visibility, f32x3 bentNormal, RWTexture2D<uint> outWorkingAOTerm )
{
    visibility = saturate( visibility / f32(XE_GTAO_OCCLUSION_TERM_SCALE) );
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
    outWorkingAOTerm[pixCoord] = XeGTAO_EncodeVisibilityBentNormal( visibility, bentNormal );
#else
    outWorkingAOTerm[pixCoord] = uint(visibility * 255.0 + 0.5);
#endif
}

float3 XeGTAO_CalculateNormal( const float4 edgesLRTB, float3 pixCenterPos, float3 pixLPos, float3 pixRPos, float3 pixTPos, float3 pixBPos )
{
    // Get this pixel's viewspace normal
    float4 acceptedNormals  = saturate( float4( edgesLRTB.x*edgesLRTB.z, edgesLRTB.z*edgesLRTB.y, edgesLRTB.y*edgesLRTB.w, edgesLRTB.w*edgesLRTB.x ) + 0.01 );

    pixLPos = normalize(pixLPos - pixCenterPos);
    pixRPos = normalize(pixRPos - pixCenterPos);
    pixTPos = normalize(pixTPos - pixCenterPos);
    pixBPos = normalize(pixBPos - pixCenterPos);

    float3 pixelNormal =  acceptedNormals.x * cross( pixLPos, pixTPos ) +
                        + acceptedNormals.y * cross( pixTPos, pixRPos ) +
                        + acceptedNormals.z * cross( pixRPos, pixBPos ) +
                        + acceptedNormals.w * cross( pixBPos, pixLPos );
    pixelNormal = normalize( pixelNormal );

    return pixelNormal;
}

float3 DisplayNormalSRGB( float3 normal )
{
    return pow( abs( normal * 0.5 + 0.5 ), 2.2 );
}

public void XeGTAO_MainPass(const uint2 pixCoord, f32 sliceCount, f32 stepsPerSlice, const f32x2 localNoise, f32x3 viewspaceNormal, const GTAOConstants consts, 
    Texture2D<f32> sourceViewspaceDepth, SamplerState depthSampler, RWTexture2D<uint> outWorkingAOTerm, RWTexture2D<unorm float> outWorkingEdges, RWTexture2D<f32x3> outNormals ) {
    float2 normalizedScreenPos = (pixCoord + 0.5.xx) * consts.ViewportPixelSize;

    constexpr i32x2 offsets[2] = {i32x2(0, 0), i32x2(1, 1)};
    f32x4 valuesUL   = gather_red(sourceViewspaceDepth, depthSampler, f32x2(pixCoord * consts.ViewportPixelSize), offsets[0]);
    f32x4 valuesBR   = gather_red(sourceViewspaceDepth, depthSampler, f32x2(pixCoord * consts.ViewportPixelSize), offsets[1]);

    // viewspace Z at the center
    f32 viewspaceZ  = valuesUL.y; //sourceViewspaceDepth.SampleLevel( depthSampler, normalizedScreenPos, 0 ).x; 

    // viewspace Zs left top right bottom
    const f32 pixLZ = valuesUL.x;
    const f32 pixTZ = valuesUL.z;
    const f32 pixRZ = valuesBR.z;
    const f32 pixBZ = valuesBR.x;

    f32x4 edgesLRTB  = XeGTAO_CalculateEdges(viewspaceZ, pixLZ, pixRZ, pixTZ, pixBZ);
    outWorkingEdges[pixCoord] = XeGTAO_PackEdges(edgesLRTB);

	// Generating screen space normals in-place is faster than generating normals in a separate pass but requires
	// use of 32bit depth buffer (16bit works but visibly degrades quality) which in turn slows everything down. So to
	// reduce complexity and allow for screen space normal reuse by other effects, we've pulled it out into a separate
	// pass.
	// However, we leave this code in, in case anyone has a use-case where it fits better.
//#ifdef XE_GTAO_GENERATE_NORMALS_INPLACE
    float3 CENTER   = XeGTAO_ComputeViewspacePosition( normalizedScreenPos, viewspaceZ, consts );
    float3 LEFT     = XeGTAO_ComputeViewspacePosition( normalizedScreenPos + float2(-1,  0) * consts.ViewportPixelSize, pixLZ, consts );
    float3 RIGHT    = XeGTAO_ComputeViewspacePosition( normalizedScreenPos + float2( 1,  0) * consts.ViewportPixelSize, pixRZ, consts );
    float3 TOP      = XeGTAO_ComputeViewspacePosition( normalizedScreenPos + float2( 0, -1) * consts.ViewportPixelSize, pixTZ, consts );
    float3 BOTTOM   = XeGTAO_ComputeViewspacePosition( normalizedScreenPos + float2( 0,  1) * consts.ViewportPixelSize, pixBZ, consts );
    viewspaceNormal = XeGTAO_CalculateNormal( edgesLRTB, CENTER, LEFT, RIGHT, TOP, BOTTOM );
//#endif

    // Move center pixel slightly towards camera to avoid imprecision artifacts due to depth buffer imprecision; offset depends on depth texture format used
    viewspaceZ *= 0.99999;     // this is good for FP32 depth buffer

    float3 pixCenterPos   = XeGTAO_ComputeViewspacePosition( normalizedScreenPos, viewspaceZ, consts );
    const f32x3 viewVec      = normalize(-pixCenterPos);

    // prevents normals that are facing away from the view vector - xeGTAO struggles with extreme cases, but in Vanilla it seems rare so it's disabled by default
    // viewspaceNormal = normalize( viewspaceNormal + max( 0, -dot( viewspaceNormal, viewVec ) ) * viewVec );

//#ifdef XE_GTAO_SHOW_NORMALS
    outNormals[pixCoord] = float3( DisplayNormalSRGB( viewspaceNormal.xyz ));
//#endif

#ifdef XE_GTAO_SHOW_EDGES
    g_outputDbgImage[pixCoord] = 1.0 - float4( edgesLRTB.x, edgesLRTB.y * 0.5 + edgesLRTB.w * 0.5, edgesLRTB.z, 1.0 );
#endif

    const f32 effectRadius              = consts.EffectRadius * consts.RadiusMultiplier;
    const f32 sampleDistributionPower   = consts.SampleDistributionPower;
    const f32 thinOccluderCompensation  = consts.ThinOccluderCompensation;
    const f32 falloffRange              = consts.EffectFalloffRange * effectRadius;

    const f32 falloffFrom       = effectRadius * (1.0f - consts.EffectFalloffRange);

    // fadeout precompute optimisation
    const f32 falloffMul        = -1.0f / ( falloffRange );
    const f32 falloffAdd        = falloffFrom / ( falloffRange ) + 1.0f;

    f32 visibility = 0;
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
    f32x3 bentNormal = 0;
#else
    f32x3 bentNormal = viewspaceNormal;
#endif

#ifdef XE_GTAO_SHOW_DEBUG_VIZ
    float3 dbgWorldPos          = mul(g_globals.ViewInv, float4(pixCenterPos, 1)).xyz;
#endif

    // see "Algorithm 1" in https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
    {
        const f32 noiseSlice  = (f32)localNoise.x;
        const f32 noiseSample = (f32)localNoise.y;

        // quality settings / tweaks / hacks
        const f32 pixelTooCloseThreshold  = 1.3;      // if the offset is under approx pixel size (pixelTooCloseThreshold), push it out to the minimum distance

        // approx viewspace pixel size at pixCoord; approximation of NDCToViewspace( normalizedScreenPos.xy + consts.ViewportPixelSize.xy, pixCenterPos.z ).xy - pixCenterPos.xy;
        const float2 pixelDirRBViewspaceSizeAtCenterZ = viewspaceZ.xx * consts.NDCToViewMul_x_PixelSize;

        f32 screenspaceRadius   = effectRadius / pixelDirRBViewspaceSizeAtCenterZ.x;

        // fade out for small screen radii 
        visibility += saturate((10 - screenspaceRadius)/100)*0.5;

#if 0   // sensible early-out for even more performance; disabled because not yet tested
        [branch]
        if( screenspaceRadius < pixelTooCloseThreshold )
        {
            XeGTAO_OutputWorkingTerm( pixCoord, 1, viewspaceNormal, outWorkingAOTerm );
            return;
        }
#endif

#ifdef XE_GTAO_SHOW_DEBUG_VIZ
        [branch] if (IsUnderCursorRange(pixCoord, int2(1, 1)))
        {
            float3 dbgWorldNorm     = mul((float3x3)g_globals.ViewInv, viewspaceNormal).xyz;
            float3 dbgWorldViewVec  = mul((float3x3)g_globals.ViewInv, viewVec).xyz;
            //DebugDraw3DArrow(dbgWorldPos, dbgWorldPos + 0.5 * dbgWorldViewVec, 0.02, float4(0, 1, 0, 0.95));
            //DebugDraw2DCircle(pixCoord, screenspaceRadius, float4(1, 0, 0.2, 1));
            DebugDraw3DSphere(dbgWorldPos, effectRadius, float4(1, 0.2, 0, 0.1));
            //DebugDraw3DText(dbgWorldPos, float2(0, 0), float4(0.6, 0.3, 0.3, 1), float4( pixelDirRBViewspaceSizeAtCenterZ.xy, 0, screenspaceRadius) );
        }
#endif

        // this is the min distance to start sampling from to avoid sampling from the center pixel (no useful data obtained from sampling center pixel)
        const f32 minS = pixelTooCloseThreshold / screenspaceRadius;

        //[unroll]
        for( f32 slice = 0; slice < sliceCount; slice++ )
        {
            f32 sliceK = (slice+noiseSlice) / sliceCount;
            // lines 5, 6 from the paper
            f32 phi = sliceK * XE_GTAO_PI;
            f32 cosPhi = cos(phi);
            f32 sinPhi = sin(phi);
            f32x2 omega = f32x2(cosPhi, -sinPhi);       //lpfloat2 on omega causes issues with big radii

            // convert to screen units (pixels) for later use
            omega *= screenspaceRadius;

            // line 8 from the paper
            const f32x3 directionVec = f32x3(cosPhi, sinPhi, 0);

            // line 9 from the paper
            const f32x3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec);

            // line 10 from the paper
            //axisVec is orthogonal to directionVec and viewVec, used to define projectedNormal
            const f32x3 axisVec = normalize( cross(orthoDirectionVec, viewVec) );

            // alternative line 9 from the paper
            // float3 orthoDirectionVec = cross( viewVec, axisVec );

            // line 11 from the paper
            f32x3 projectedNormalVec = viewspaceNormal - axisVec * dot(viewspaceNormal, axisVec);

            // line 13 from the paper
            f32 signNorm = (f32)sign( dot( orthoDirectionVec, projectedNormalVec ) );

            // line 14 from the paper
            f32 projectedNormalVecLength = length(projectedNormalVec);
            f32 cosNorm = saturate(dot(projectedNormalVec, viewVec) / projectedNormalVecLength);

            // line 15 from the paper
            f32 n = signNorm * XeGTAO_FastACos(cosNorm);

            // this is a lower weight target; not using -1 as in the original paper because it is under horizon, so a 'weight' has different meaning based on the normal
            const f32 lowHorizonCos0  = cos(n+XE_GTAO_PI_HALF);
            const f32 lowHorizonCos1  = cos(n-XE_GTAO_PI_HALF);

            // lines 17, 18 from the paper, manually unrolled the 'side' loop
            f32 horizonCos0           = lowHorizonCos0; //-1;
            f32 horizonCos1           = lowHorizonCos1; //-1;

            for( f32 step = 0; step < stepsPerSlice; step++ )
            {
                // R1 sequence (http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/)
                const f32 stepBaseNoise = f32(slice + step * stepsPerSlice) * 0.6180339887498948482; // <- this should unroll
                f32 stepNoise = frac(noiseSample + stepBaseNoise);

                // approx line 20 from the paper, with added noise
                f32 s = (step+stepNoise) / (stepsPerSlice); // + (lpfloat2)1e-6f);

                // additional distribution modifier
                s       = (f32)pow( s, (f32)sampleDistributionPower );

                // avoid sampling center pixel
                s       += minS;

                // approx lines 21-22 from the paper, unrolled
                f32x2 sampleOffset = s * omega;

                f32 sampleOffsetLength = length( sampleOffset );

                // note: when sampling, using point_point_point or point_point_linear sampler works, but linear_linear_linear will cause unwanted interpolation between neighbouring depth values on the same MIP level!
                const f32 mipLevel    = (f32)clamp( log2( sampleOffsetLength ) - consts.DepthMIPSamplingOffset, 0, XE_GTAO_DEPTH_MIP_LEVELS );

                // Snap to pixel center (more correct direction math, avoids artifacts due to sampling pos not matching depth texel center - messes up slope - but adds other 
                // artifacts due to them being pushed off the slice). Also use full precision for high res cases.
                sampleOffset = round(sampleOffset) * (f32x2)consts.ViewportPixelSize;

#ifdef XE_GTAO_SHOW_DEBUG_VIZ
                int mipLevelU = (int)round(mipLevel);
                float4 mipColor = saturate( float4( mipLevelU>=3, mipLevelU>=1 && mipLevelU<=3, mipLevelU<=1, 1.0 ) );
                if( all( sampleOffset == 0 ) )
                    DebugDraw2DText( pixCoord, float4( 1, 0, 0, 1), pixelTooCloseThreshold );
                [branch] if (IsUnderCursorRange(pixCoord, int2(1, 1)))
                {
                    //DebugDraw2DText( (normalizedScreenPos + sampleOffset) * consts.ViewportSize, mipColor, mipLevelU );
                    //DebugDraw2DText( (normalizedScreenPos + sampleOffset) * consts.ViewportSize, mipColor, (uint)slice );
                    //DebugDraw2DText( (normalizedScreenPos - sampleOffset) * consts.ViewportSize, mipColor, (uint)slice );
                    //DebugDraw2DText( (normalizedScreenPos - sampleOffset) * consts.ViewportSize, saturate( float4( mipLevelU>=3, mipLevelU>=1 && mipLevelU<=3, mipLevelU<=1, 1.0 ) ), mipLevelU );
                }
#endif

                float2 sampleScreenPos0 = normalizedScreenPos + sampleOffset;
                float  SZ0 = sourceViewspaceDepth.SampleLevel( depthSampler, sampleScreenPos0, mipLevel ).x;
                float3 samplePos0 = XeGTAO_ComputeViewspacePosition( sampleScreenPos0, SZ0, consts );

                float2 sampleScreenPos1 = normalizedScreenPos - sampleOffset;
                float  SZ1 = sourceViewspaceDepth.SampleLevel( depthSampler, sampleScreenPos1, mipLevel ).x;
                float3 samplePos1 = XeGTAO_ComputeViewspacePosition( sampleScreenPos1, SZ1, consts );

                float3 sampleDelta0     = (samplePos0 - float3(pixCenterPos)); // using lpfloat for sampleDelta causes precision issues
                float3 sampleDelta1     = (samplePos1 - float3(pixCenterPos)); // using lpfloat for sampleDelta causes precision issues
                f32 sampleDist0     = length( sampleDelta0 );
                f32 sampleDist1     = length( sampleDelta1 );

                // approx lines 23, 24 from the paper, unrolled
                f32x3 sampleHorizonVec0 = (sampleDelta0 / sampleDist0);
                f32x3 sampleHorizonVec1 = (sampleDelta1 / sampleDist1);

                // any sample out of radius should be discarded - also use fallof range for smooth transitions; this is a modified idea from "4.3 Implementation details, Bounding the sampling area"

                // this is our own thickness heuristic that relies on sooner discarding samples behind the center
                f32 falloffBase0    = length( f32x3(sampleDelta0.x, sampleDelta0.y, sampleDelta0.z * (1+thinOccluderCompensation) ) );
                f32 falloffBase1    = length( f32x3(sampleDelta1.x, sampleDelta1.y, sampleDelta1.z * (1+thinOccluderCompensation) ) );
                f32 weight0         = saturate( falloffBase0 * falloffMul + falloffAdd );
                f32 weight1         = saturate( falloffBase1 * falloffMul + falloffAdd );

                // sample horizon cos
                f32 shc0 = dot(sampleHorizonVec0, viewVec);
                f32 shc1 = dot(sampleHorizonVec1, viewVec);

                // discard unwanted samples
                shc0 = lerp( lowHorizonCos0, shc0, weight0 ); // this would be more correct but too expensive: cos(lerp( acos(lowHorizonCos0), acos(shc0), weight0 ));
                shc1 = lerp( lowHorizonCos1, shc1, weight1 ); // this would be more correct but too expensive: cos(lerp( acos(lowHorizonCos1), acos(shc1), weight1 ));

                // thickness heuristic - see "4.3 Implementation details, Height-field assumption considerations"
#if 0   // (disabled, not used) this should match the paper
                f32 newhorizonCos0 = max( horizonCos0, shc0 );
                f32 newhorizonCos1 = max( horizonCos1, shc1 );
                horizonCos0 = (horizonCos0 > shc0)?( lerp( newhorizonCos0, shc0, thinOccluderCompensation ) ):( newhorizonCos0 );
                horizonCos1 = (horizonCos1 > shc1)?( lerp( newhorizonCos1, shc1, thinOccluderCompensation ) ):( newhorizonCos1 );
#elif 0 // (disabled, not used) this is slightly different from the paper but cheaper and provides very similar results
                horizonCos0 = lerp( max( horizonCos0, shc0 ), shc0, thinOccluderCompensation );
                horizonCos1 = lerp( max( horizonCos1, shc1 ), shc1, thinOccluderCompensation );
#else   // this is a version where thicknessHeuristic is completely disabled
                horizonCos0 = max( horizonCos0, shc0 );
                horizonCos1 = max( horizonCos1, shc1 );
#endif


#ifdef XE_GTAO_SHOW_DEBUG_VIZ
                [branch] if (IsUnderCursorRange(pixCoord, int2(1, 1)))
                {
                    float3 WS_samplePos0 = mul(g_globals.ViewInv, float4(samplePos0, 1)).xyz;
                    float3 WS_samplePos1 = mul(g_globals.ViewInv, float4(samplePos1, 1)).xyz;
                    float3 WS_sampleHorizonVec0 = mul( (float3x3)g_globals.ViewInv, sampleHorizonVec0).xyz;
                    float3 WS_sampleHorizonVec1 = mul( (float3x3)g_globals.ViewInv, sampleHorizonVec1).xyz;
                    // DebugDraw3DSphere( WS_samplePos0, effectRadius * 0.02, DbgGetSliceColor(slice, sliceCount, false) );
                    // DebugDraw3DSphere( WS_samplePos1, effectRadius * 0.02, DbgGetSliceColor(slice, sliceCount, true) );
                    DebugDraw3DSphere( WS_samplePos0, effectRadius * 0.02, mipColor );
                    DebugDraw3DSphere( WS_samplePos1, effectRadius * 0.02, mipColor );
                    // DebugDraw3DArrow( WS_samplePos0, WS_samplePos0 - WS_sampleHorizonVec0, 0.002, float4(1, 0, 0, 1 ) );
                    // DebugDraw3DArrow( WS_samplePos1, WS_samplePos1 - WS_sampleHorizonVec1, 0.002, float4(1, 0, 0, 1 ) );
                    // DebugDraw3DText( WS_samplePos0, float2(0,  0), float4( 1, 0, 0, 1), weight0 );
                    // DebugDraw3DText( WS_samplePos1, float2(0,  0), float4( 1, 0, 0, 1), weight1 );

                    // DebugDraw2DText( float2( 500, 94+(step+slice*3)*12 ), float4( 0, 1, 0, 1 ), float4( projectedNormalVecLength, 0, horizonCos0, horizonCos1 ) );
                }
#endif
            }

#if 1       // I can't figure out the slight overdarkening on high slopes, so I'm adding this fudge - in the training set, 0.05 is close (PSNR 21.34) to disabled (PSNR 21.45)
            projectedNormalVecLength = lerp( projectedNormalVecLength, 1, 0.05 );
#endif

            // line ~27, unrolled
            f32 h0 = -XeGTAO_FastACos((f32)horizonCos1);
            f32 h1 = XeGTAO_FastACos((f32)horizonCos0);
#if 0       // we can skip clamping for a tiny little bit more performance
            h0 = n + clamp( h0-n, (f32)-XE_GTAO_PI_HALF, (f32)XE_GTAO_PI_HALF );
            h1 = n + clamp( h1-n, (f32)-XE_GTAO_PI_HALF, (f32)XE_GTAO_PI_HALF );
#endif
            f32 iarc0 = ((f32)cosNorm + (f32)2 * (f32)h0 * (f32)sin(n)-(f32)cos((f32)2 * (f32)h0-n))/(f32)4;
            f32 iarc1 = ((f32)cosNorm + (f32)2 * (f32)h1 * (f32)sin(n)-(f32)cos((f32)2 * (f32)h1-n))/(f32)4;
            f32 localVisibility = (f32)projectedNormalVecLength * (f32)(iarc0+iarc1);
            visibility += localVisibility;

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
            // see "Algorithm 2 Extension that computes bent normals b."
            f32 t0 = (6*sin(h0-n)-sin(3*h0-n)+6*sin(h1-n)-sin(3*h1-n)+16*sin(n)-3*(sin(h0+n)+sin(h1+n)))/12;
            f32 t1 = (-cos(3 * h0-n)-cos(3 * h1-n) +8 * cos(n)-3 * (cos(h0+n) +cos(h1+n)))/12;
            f32x3 localBentNormal = f32x3( directionVec.x * (f32)t0, directionVec.y * (f32)t0, -f32(t1) );
            localBentNormal = (f32x3)mul( XeGTAO_RotFromToMatrix( f32x3(0,0,-1), viewVec ), localBentNormal ) * projectedNormalVecLength;
            bentNormal += localBentNormal;
#endif
        }
        visibility /= sliceCount;
        visibility = pow(visibility, consts.FinalValuePower);
        visibility = max(0.03f, visibility); // disallow total occlusion (which wouldn't make any sense anyhow since pixel is visible but also helps with packing bent normals)

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
        bentNormal = normalize(bentNormal) ;
#endif
    }

#if defined(XE_GTAO_SHOW_DEBUG_VIZ) && defined(XE_GTAO_COMPUTE_BENT_NORMALS)
    [branch] if (IsUnderCursorRange(pixCoord, int2(1, 1)))
    {
        float3 dbgWorldViewNorm = mul((float3x3)g_globals.ViewInv, viewspaceNormal).xyz;
        float3 dbgWorldBentNorm = mul((float3x3)g_globals.ViewInv, bentNormal).xyz;
        DebugDraw3DSphereCone( dbgWorldPos, dbgWorldViewNorm, 0.3, VA_PI*0.5 - acos(saturate(visibility)), float4( 0.2, 0.2, 0.2, 0.5 ) );
        DebugDraw3DSphereCone( dbgWorldPos, dbgWorldBentNorm, 0.3, VA_PI*0.5 - acos(saturate(visibility)), float4( 0.0, 1.0, 0.0, 0.7 ) );
    }
#endif

    XeGTAO_OutputWorkingTerm( pixCoord, visibility, bentNormal, outWorkingAOTerm );
}

void XeGTAO_DecodeGatherPartial( const uint4 packedValue, out AOTermType outDecoded[4] )
{
    for( int i = 0; i < 4; i++ )
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
        XeGTAO_DecodeVisibilityBentNormal( packedValue[i], outDecoded[i].w, outDecoded[i].xyz );
#else
        outDecoded[i] = f32(packedValue[i]) / f32(255.0);
#endif
}

void XeGTAO_AddSample(AOTermType ssaoValue, f32 edgeValue, inout AOTermType sum, inout f32 sumWeight) {
    f32 weight = edgeValue;

    sum += (weight * ssaoValue);
    sumWeight += weight;
}

f32x4 XeGTAO_UnpackEdges(f32 _packedVal) {
    uint packedVal = (uint)(_packedVal * 255.5);
    f32x4 edgesLRTB;
    edgesLRTB.x = f32((packedVal >> 6) & 0x03) / 3.0;          // there's really no need for mask (as it's an 8 bit input) but I'll leave it in so it doesn't cause any trouble in the future
    edgesLRTB.y = f32((packedVal >> 4) & 0x03) / 3.0;
    edgesLRTB.z = f32((packedVal >> 2) & 0x03) / 3.0;
    edgesLRTB.w = f32((packedVal >> 0) & 0x03) / 3.0;

    return saturate( edgesLRTB );
}

void XeGTAO_Output(uint2 pixCoord, RWTexture2D<uint> outputTexture, AOTermType outputValue, const uniform bool finalApply) {
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
    f32     visibility = outputValue.w * ((finalApply)?((f32)XE_GTAO_OCCLUSION_TERM_SCALE):(1));
    f32x3    bentNormal = normalize(outputValue.xyz);
    outputTexture[pixCoord.xy] = XeGTAO_EncodeVisibilityBentNormal(visibility, bentNormal);
#else
    outputValue *=  (finalApply)?((f32)XE_GTAO_OCCLUSION_TERM_SCALE):(1);
    outputTexture[pixCoord.xy] = uint(outputValue * 255.0 + 0.5);
#endif
}

public void XeGTAO_Denoise(const uint2 pixCoordBase, const GTAOConstants consts, Texture2D<uint> sourceAOTerm, Texture2D<f32> sourceEdges, SamplerState texSampler, RWTexture2D<uint> outputTexture, const uniform bool finalApply) {
    const f32 blurAmount = (finalApply)?((f32)consts.DenoiseBlurBeta):((f32)consts.DenoiseBlurBeta/(f32)5.0);
    const f32 diagWeight = 0.85 * 0.5;

    AOTermType aoTerm[2];   // pixel pixCoordBase and pixel pixCoordBase + int2( 1, 0 )
    f32x4 edgesC_LRTB[2];
    f32 weightTL[2];
    f32 weightTR[2];
    f32 weightBL[2];
    f32 weightBR[2];

    // gather edge and visibility quads, used later
    const float2 gatherCenter = float2( pixCoordBase.x, pixCoordBase.y ) * consts.ViewportPixelSize;

    constexpr i32x2 offsets[3] = {i32x2(0, 0), i32x2(2, 0), i32x2(1, 2)};
    f32x4 edgesQ0   = gather_red(sourceEdges, texSampler, gatherCenter, offsets[0]);
    f32x4 edgesQ1   = gather_red(sourceEdges, texSampler, gatherCenter, offsets[1]);
    f32x4 edgesQ2   = gather_red(sourceEdges, texSampler, gatherCenter, offsets[2]);

    constexpr i32x2 offsets_u[4] = {i32x2(0, 0), i32x2(2, 0), i32x2(0, 2), i32x2(2, 2)};

    AOTermType visQ0[4];    XeGTAO_DecodeGatherPartial(gather_red_u(sourceAOTerm, texSampler, gatherCenter, offsets_u[0]), visQ0);
    AOTermType visQ1[4];    XeGTAO_DecodeGatherPartial(gather_red_u(sourceAOTerm, texSampler, gatherCenter, offsets_u[1]), visQ1);
    AOTermType visQ2[4];    XeGTAO_DecodeGatherPartial(gather_red_u(sourceAOTerm, texSampler, gatherCenter, offsets_u[2]), visQ2);
    AOTermType visQ3[4];    XeGTAO_DecodeGatherPartial(gather_red_u(sourceAOTerm, texSampler, gatherCenter, offsets_u[3]), visQ3);

    for( int side = 0; side < 2; side++ )
    {
        const int2 pixCoord = int2( pixCoordBase.x + side, pixCoordBase.y );

        f32x4 edgesL_LRTB  = XeGTAO_UnpackEdges( (side==0)?(edgesQ0.x):(edgesQ0.y) );
        f32x4 edgesT_LRTB  = XeGTAO_UnpackEdges( (side==0)?(edgesQ0.z):(edgesQ1.w) );
        f32x4 edgesR_LRTB  = XeGTAO_UnpackEdges( (side==0)?(edgesQ1.x):(edgesQ1.y) );
        f32x4 edgesB_LRTB  = XeGTAO_UnpackEdges( (side==0)?(edgesQ2.w):(edgesQ2.z) );

        edgesC_LRTB[side]     = XeGTAO_UnpackEdges( (side==0)?(edgesQ0.y):(edgesQ1.x) );

        // Edges aren't perfectly symmetrical: edge detection algorithm does not guarantee that a left edge on the right pixel will match the right edge on the left pixel (although
        // they will match in majority of cases). This line further enforces the symmetricity, creating a slightly sharper blur. Works real nice with TAA.
        edgesC_LRTB[side] *= f32x4( edgesL_LRTB.y, edgesR_LRTB.x, edgesT_LRTB.w, edgesB_LRTB.z );

#if 1   // this allows some small amount of AO leaking from neighbours if there are 3 or 4 edges; this reduces both spatial and temporal aliasing
        const f32 leak_threshold = 2.5; const f32 leak_strength = 0.5;
        f32 edginess = (saturate(4.0 - leak_threshold - dot( edgesC_LRTB[side], 1.xxxx )) / (4-leak_threshold)) * leak_strength;
        edgesC_LRTB[side] = saturate( edgesC_LRTB[side] + edginess );
#endif

#ifdef XE_GTAO_SHOW_EDGES
        g_outputDbgImage[pixCoord] = 1.0 - f32x4( edgesC_LRTB[side].x, edgesC_LRTB[side].y * 0.5 + edgesC_LRTB[side].w * 0.5, edgesC_LRTB[side].z, 1.0 );
        //g_outputDbgImage[pixCoord] = 1 - float4( edgesC_LRTB[side].z, edgesC_LRTB[side].w , 1, 0 );
        //g_outputDbgImage[pixCoord] = edginess.xxxx;
#endif

        // for diagonals; used by first and second pass
        weightTL[side] = diagWeight * (edgesC_LRTB[side].x * edgesL_LRTB.z + edgesC_LRTB[side].z * edgesT_LRTB.x);
        weightTR[side] = diagWeight * (edgesC_LRTB[side].z * edgesT_LRTB.y + edgesC_LRTB[side].y * edgesR_LRTB.z);
        weightBL[side] = diagWeight * (edgesC_LRTB[side].w * edgesB_LRTB.x + edgesC_LRTB[side].x * edgesL_LRTB.w);
        weightBR[side] = diagWeight * (edgesC_LRTB[side].y * edgesR_LRTB.w + edgesC_LRTB[side].w * edgesB_LRTB.y);

        // first pass
        AOTermType ssaoValue     = (side==0)?(visQ0[1]):(visQ1[0]);
        AOTermType ssaoValueL    = (side==0)?(visQ0[0]):(visQ0[1]);
        AOTermType ssaoValueT    = (side==0)?(visQ0[2]):(visQ1[3]);
        AOTermType ssaoValueR    = (side==0)?(visQ1[0]):(visQ1[1]);
        AOTermType ssaoValueB    = (side==0)?(visQ2[2]):(visQ3[3]);
        AOTermType ssaoValueTL   = (side==0)?(visQ0[3]):(visQ0[2]);
        AOTermType ssaoValueBR   = (side==0)?(visQ3[3]):(visQ3[2]);
        AOTermType ssaoValueTR   = (side==0)?(visQ1[3]):(visQ1[2]);
        AOTermType ssaoValueBL   = (side==0)?(visQ2[3]):(visQ2[2]);

        f32 sumWeight = blurAmount;
        AOTermType sum = ssaoValue * sumWeight;

        XeGTAO_AddSample( ssaoValueL, edgesC_LRTB[side].x, sum, sumWeight );
        XeGTAO_AddSample( ssaoValueR, edgesC_LRTB[side].y, sum, sumWeight );
        XeGTAO_AddSample( ssaoValueT, edgesC_LRTB[side].z, sum, sumWeight );
        XeGTAO_AddSample( ssaoValueB, edgesC_LRTB[side].w, sum, sumWeight );

        XeGTAO_AddSample( ssaoValueTL, weightTL[side], sum, sumWeight );
        XeGTAO_AddSample( ssaoValueTR, weightTR[side], sum, sumWeight );
        XeGTAO_AddSample( ssaoValueBL, weightBL[side], sum, sumWeight );
        XeGTAO_AddSample( ssaoValueBR, weightBR[side], sum, sumWeight );

        aoTerm[side] = sum / sumWeight;

        XeGTAO_Output( pixCoord, outputTexture, aoTerm[side], finalApply );

#ifdef XE_GTAO_SHOW_BENT_NORMALS
        if( finalApply )
        {
            g_outputDbgImage[pixCoord] = float4( DisplayNormalSRGB( aoTerm[side].xyz /** aoTerm[side].www*/ ), 1 );
        }
#endif

    }
}