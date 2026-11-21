import common;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Texture2D<f32x4> accumulation_image;
[[vk::binding(1, 0)]] Texture2D<f32> revealage_image;

struct ResolveOutput {
    f32x4 color : SV_Target0;
    f32 reactive : SV_Target1;
};

[[shader("fragment")]]
func fs_main(VertexOutput input) -> ResolveOutput {
    let pixel = u32x3(u32x2(input.position.xy), 0);
    let revealage = revealage_image.Load(pixel);
    if (revealage >= 1.0) {
        discard;
    }

    let accumulation = accumulation_image.Load(pixel);
    let average = accumulation.rgb / max(accumulation.a, 1e-5);
    let coverage = 1.0 - revealage;

    ResolveOutput output;
    output.color = f32x4(average * coverage, coverage);
    output.reactive = min(coverage, 0.9);

    return output;
}
