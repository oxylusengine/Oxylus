import common;
import gpu;
import scene;
import rmvsm;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> normal_image;
// CSM
// [[vk::binding(0, 1)]] Texture2DArray<f32> csm_cascades;
// VSM
[[vk::binding(0, 2)]] StructuredBuffer<VirtualClipmap> clipmaps;
[[vk::binding(1, 2)]] Texture2DArray<u32> vsm_page_table;
[[vk::binding(2, 2)]] Texture2D<f32> vsm_physical_pages;

[[shader("fragment")]]
func fs_main(
    VertexOutput input,
    uniform VSMContext vsm_ctx
) -> f32 {
    let pixel_pos = i32x3(i32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);
    if (depth == 0.0) {
        return 1.0;
    }

    let mapped_smooth_normal = normal_image.Load(pixel_pos);
    let N = normalize(com::oct_to_vec3(mapped_smooth_normal.rg));
    let L = vsm_ctx.directional_light_dir;
    let NoL = max(dot(N, L), 0.0);

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let clipmap_index = vsm_ctx.get_clipmap_index(camera, input.tex_coord, depth);
    let clipmap = clipmaps[clipmap_index];
    let clipmap_info = clipmap.get_clipmap_info(world_position);

    let virt_coords = clipmap_info.get_virt_coords(vsm_ctx.get_page_table_extent().xy);
    let wrapped_coords = vsm_ctx.page_coords_virtual_to_wrapped(virt_coords, clipmap.page_offset);

    if (any(wrapped_coords == INVALID_COORDS)) {
        return 1.0;
    }

    let page_address = i32x4(wrapped_coords, clipmap_index, 0);
    let page = VSMPageMetadata(vsm_page_table.Load(page_address));

    if (!page.is_backed()) {
        return 1.0;
    }

    let physical_page_coords = i32x2(page.get_physical_coords(vsm_ctx.physcial_page_table_size, vsm_ctx.page_size));
    let phys_texel = clipmap_info.get_virt_coords(vsm_ctx.get_physical_page_table_extent().xy);
    let in_page_texel = i32x2(com::mod(f32x2(phys_texel), f32x2(vsm_ctx.page_size)));
    let physical_texel_coord = physical_page_coords * vsm_ctx.page_size + in_page_texel;

    let shadow_depth = vsm_physical_pages.Load(i32x3(physical_texel_coord, 0));
    let max_bias = exp2(f32(clipmap_index)) * 0.1;
    var bias = min(
        max_bias,
        0.01 + calc_shadow_bias(clipmap_index, vsm_ctx.get_first_clipmap_texel_length(), N, L) / NoL);
        bias = bias / vsm_ctx.z_length;
    return ((shadow_depth + bias) < clipmap_info.projected_depth) ? 0.0 : 1.0;
}
