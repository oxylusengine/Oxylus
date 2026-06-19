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

// TODO: Make these tweakable
static const u32 PCSS_BLOCKER_SEARCH_SAMPLES = 16;
static const u32 PCSS_PCF_SAMPLES = 24;
static const f32 PCSS_BLOCKER_SEARCH_RADIUS = 0.1;
static const f32 PCSS_LIGHT_WIDTH = 0.002;
static const f32 PCSS_MAX_PCF_RADIUS = 0.1;

static const f32 VSM_DEPTH_MISS = -1.0;

func hash2(f32x2 p) -> f32x2 {
    let d = f32x2(dot(p, f32x2(127.1, 311.7)), dot(p, f32x2(269.5, 183.3)));
    return fract(sin(d) * 43758.5453);
}

func calc_vsm_shadow_bias(in VSMContext vsm_ctx, u32 clipmap_index, f32x3 N, f32x3 L, f32 NoL) -> f32 {
    return calc_shadow_bias(clipmap_index, vsm_ctx.get_first_clipmap_texel_length(), N, L, NoL);
}

func sample_vsm_shadow_depth(
    in VSMContext vsm_ctx,
    i32 clipmap_index,
    f32x3 world_pos
) -> f32 {
    if (clipmap_index < 0 || clipmap_index >= vsm_ctx.clipmap_count) {
        return VSM_DEPTH_MISS;
    }

    let clipmap = clipmaps[clipmap_index];
    let clipmap_info = clipmap.get_clipmap_info(world_pos);

    let virt_coords = clipmap_info.get_virt_coords(vsm_ctx.get_page_table_extent().xy);
    let wrapped_coords = vsm_ctx.page_coords_virtual_to_wrapped(virt_coords, clipmap.page_offset);
    if (any(wrapped_coords == INVALID_COORDS)) {
        return VSM_DEPTH_MISS;
    }

    let page = VSMPageMetadata(vsm_page_table.Load(i32x4(wrapped_coords, clipmap_index, 0)));
    if (!page.is_backed()) {
        return VSM_DEPTH_MISS;
    }

    let physical_page_coords = i32x2(page.get_physical_coords(vsm_ctx.physcial_page_table_size, vsm_ctx.page_size));
    let phys_texel = clipmap_info.get_virt_coords(vsm_ctx.get_physical_page_table_extent().xy);
    let in_page_texel = i32x2(com::mod(f32x2(phys_texel), f32x2(vsm_ctx.page_size)));
    let physical_texel_coord = physical_page_coords * vsm_ctx.page_size + in_page_texel;

    return vsm_physical_pages.Load(i32x3(physical_texel_coord, 0));
}

func sample_vsm_shadow_depth_with_fallback(
    in VSMContext vsm_ctx,
    i32 base_clipmap_index,
    f32x3 world_pos
) -> f32 {
    let d0 = sample_vsm_shadow_depth(vsm_ctx, base_clipmap_index, world_pos);
    if (d0 != VSM_DEPTH_MISS) { return d0; }
    let d1 = sample_vsm_shadow_depth(vsm_ctx, base_clipmap_index - 1, world_pos);
    if (d1 != VSM_DEPTH_MISS) { return d1; }
    let d2 = sample_vsm_shadow_depth(vsm_ctx, base_clipmap_index + 1, world_pos);
    if (d2 != VSM_DEPTH_MISS) { return d2; }
    return VSM_DEPTH_MISS;
}

func perpendicular_basis(f32x3 L, out f32x3 tangent, out f32x3 bitangent) -> void {
    let axis = abs(L.y) < 0.999 ? f32x3(0.0, 1.0, 0.0) : f32x3(1.0, 0.0, 0.0);
    let t = normalize(cross(axis, L));
    tangent = t;
    bitangent = cross(L, t);
}

func pcss_shadow(
    in VSMContext vsm_ctx,
    i32 base_clipmap_index,
    f32x3 world_position,
    f32x3 flat_normal,
    f32x2 noise
) -> f32 {
    let L = vsm_ctx.directional_light_dir;
    let NoL = max(dot(flat_normal, L), 0.0);

    let max_bias = exp2(f32(base_clipmap_index)) * 0.02;
    let base_bias = min(max_bias, 0.01 + calc_vsm_shadow_bias(vsm_ctx, u32(base_clipmap_index), flat_normal, L, NoL));
    let inv_z_length = 1.0 / vsm_ctx.z_length;

    var tangent = f32x3(0.0);
    var bitangent = f32x3(0.0);
    perpendicular_basis(L, tangent, bitangent);

    let receiver_info = clipmaps[base_clipmap_index].get_clipmap_info(world_position);
    let d_receiver_norm = receiver_info.projected_depth;
    let d_receiver_world = d_receiver_norm * vsm_ctx.z_length;

    let center_depth = sample_vsm_shadow_depth_with_fallback(vsm_ctx, base_clipmap_index, world_position);

    // Blocker search
    var accum_depth_world = 0.0;
    var blockers = 0u;
    var valid_blocker_samples = 0u;
    for (var i = 0u; i < PCSS_BLOCKER_SEARCH_SAMPLES; i += 1u) {
        let xi = fract(com::hammersley2d(i, PCSS_BLOCKER_SEARCH_SAMPLES) + noise);
        let r = sqrt(xi.x) * PCSS_BLOCKER_SEARCH_RADIUS;
        let theta = xi.y * TAU;
        let offset = r * (tangent * cos(theta) + bitangent * sin(theta));

        let pcf_bias = 2.0 * r;
        let bias_norm = inv_z_length * (base_bias + lerp(pcf_bias, 0.0, NoL));

        let depth = sample_vsm_shadow_depth_with_fallback(vsm_ctx, base_clipmap_index, world_position + offset);
        if (depth == VSM_DEPTH_MISS) {
            continue;
        }
        valid_blocker_samples += 1u;

        if (depth + bias_norm < d_receiver_norm) {
            accum_depth_world += depth * vsm_ctx.z_length;
            blockers += 1u;
        }
    }

    let hard_bias_norm = inv_z_length * base_bias;
    let hard_shadow = (center_depth == VSM_DEPTH_MISS)
        ? 1.0
        : ((center_depth + hard_bias_norm < d_receiver_norm) ? 0.0 : 1.0);

    if (valid_blocker_samples == 0u) {
        return hard_shadow;
    }
    if (blockers == 0u) {
        return 1.0;
    }
    if (blockers == valid_blocker_samples) {
        return 0.0;
    }

    let d_blocker_world = accum_depth_world / f32(blockers);
    let pcf_radius = min(PCSS_MAX_PCF_RADIUS, (d_receiver_world - d_blocker_world) * PCSS_LIGHT_WIDTH);

    // PCF
    var light_visibility = 0.0;
    var valid_pcf_samples = 0u;
    for (var i = 0u; i < PCSS_PCF_SAMPLES; i += 1u) {
        let xi = fract(com::hammersley2d(i, PCSS_PCF_SAMPLES) + noise.yx);
        let r = sqrt(xi.x) * pcf_radius;
        let theta = xi.y * TAU;
        let offset = r * (tangent * cos(theta) + bitangent * sin(theta));

        let pcf_bias = 2.0 * r;
        let bias_norm = inv_z_length * (base_bias + lerp(pcf_bias, 0.0, NoL));

        let depth = sample_vsm_shadow_depth_with_fallback(vsm_ctx, base_clipmap_index, world_position + offset);
        if (depth == VSM_DEPTH_MISS) {
            continue;
        }
        valid_pcf_samples += 1u;

        if (depth + bias_norm >= d_receiver_norm) {
            light_visibility += 1.0;
        }
    }

    if (valid_pcf_samples == 0u) {
        return hard_shadow;
    }
    return light_visibility / f32(valid_pcf_samples);
}

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
    let flat_N = normalize(com::oct_to_vec3(mapped_smooth_normal.ba));

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, depth);
    let world_position_h = mul(camera.inv_projection_view, f32x4(NDC, 1.0));
    let world_position = world_position_h.xyz / world_position_h.w;

    let clipmap_index = i32(vsm_ctx.get_clipmap_index(camera, input.tex_coord, depth));

    let noise = hash2(f32x2(input.position.xy));
    return pcss_shadow(vsm_ctx, clipmap_index, world_position, flat_N, noise);
}
