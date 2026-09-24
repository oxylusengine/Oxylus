// AMD FidelityFX Super Resolution 3.1.5 upscaler, ported from the MIT licensed HLSL in
// FidelityFX-SDK Kits/FidelityFX/upscalers/fsr3. The SDK compiles a permutation matrix out of
// these sources; oxylus only ever needs one configuration, so the port hardcodes it:
//
//   hdr color input      : the upscaler runs on linear hdr, before tonemapping
//   low res motion vecs  : velocity is written at render resolution by the visbuffer decode
//   unjittered motion    : jitter is applied to clip position only, never to the mv matrices
//   inverted depth       : oxylus is reversed-z
//
// Anything guarded by the other permutation defines is dropped rather than carried as dead branches.
module fsr3;

import common;

__include fsr3.constants;
__include fsr3.common;
__include fsr3.reproject;
__include fsr3.sample;
__include fsr3.upsample;
__include fsr3.accumulate;

