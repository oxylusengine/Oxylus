import common;
import gpu;
import scene;
import rmvsm;

#include <fullscreen.slang>

[[vk::binding(0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1)]] StructuredBuffer<VirtualClipmap> clipmaps;
[[vk::binding(2)]] Texture2D<f32> depth_image;
[[vk::binding(3)]] Texture2D<f32x4> normal_image;
[[vk::binding(4)]] Texture2DArray<u32> vsm_page_table;

func hsv_to_rgb(f32x3 hsv) -> f32x3 {
    let n = f32x3(5.0, 3.0, 1.0);
    let k = (n + hsv.x / 1.0471975512) % 6.0;
    return hsv.z - hsv.z * hsv.y * max(f32x3(0.0), min(k, min(4.0 - k, f32x3(1.0))));
}
[[shader("fragment")]]
func fs_main(
    VertexOutput input,
    uniform VSMContext vsm_ctx
) -> f32x4 {
    let pixel_pos = i32x3(i32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);
    if (depth == 0.0) {
        return 1.0;
    }

    let mapped_smooth_normal = normal_image.Load(pixel_pos);
    let flat_N = normalize(com::oct_to_vec3(mapped_smooth_normal.ba));

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let clipmap_index = i32(vsm_ctx.get_clipmap_index(camera, input.tex_coord, depth));
    let clipmap = clipmaps[clipmap_index];
    let clipmap_info = clipmap.get_clipmap_info(world_position);

    let virt_coords = clipmap_info.get_virt_coords(vsm_ctx.get_page_table_extent().xy);
    let wrapped_coords = vsm_ctx.page_coords_virtual_to_wrapped(virt_coords, clipmap.page_offset);
    let page = VSMPageMetadata(vsm_page_table.Load(i32x4(wrapped_coords, clipmap_index, 0)));
    let physical_page_coords = i32x2(page.get_physical_coords(vsm_ctx.physcial_page_table_size, vsm_ctx.page_size));
    let phys_texel = clipmap_info.get_virt_coords(vsm_ctx.get_physical_page_table_extent().xy);
    let in_page_texel = i32x2(com::mod(f32x2(phys_texel), f32x2(vsm_ctx.page_size)));
    let physical_texel_coord = physical_page_coords * vsm_ctx.page_size + in_page_texel;
    let near_border = any(in_page_texel < 2) || any(in_page_texel > vsm_ctx.page_size - 2);

    // page border highlighting
    if (near_border) {
      let line_hsv = f32x3(f32(clipmap_index) / f32(vsm_ctx.clipmap_count + 1) * TAU, 1.0, 0.5);
      let clipmap_color = hsv_to_rgb(line_hsv);
      return f32x4(clipmap_color, 1.0);
    }

    let page_status = f32x3(f32(page.is_backed()), f32(page.is_dirty()), f32(page.is_visible()));
    return f32x4(page_status, 1.0);
}
