import common;
import gpu;

#include <fullscreen.slang>

// Point samples render resolution depth into a display resolution depth target. Overlay passes that
// depth test against the scene (the editor grid, for one) bind depth as a real attachment next to
// the display resolution color target, and vulkan requires matching framebuffer extents. Overlays
// do not need subpixel accurate depth, so nearest is enough.
struct ShaderParameters {
    Texture2D<f32> src_depth;
    Sampler sampler_nearest_clamp;
};
ParameterBlock<ShaderParameters> params;

struct FragmentOutput {
    f32 depth : SV_Depth;
};

[[shader("fragment")]]
func fs_main(VertexOutput input) -> FragmentOutput {
    FragmentOutput output;
    output.depth = params.src_depth.SampleLevel(params.sampler_nearest_clamp, input.tex_coord, 0.0);
    return output;
}
