import common;
import gpu;
import scene;
import rmvsm;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] Texture2D<f32> depth_image;
[[vk::binding(2, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(0, 2)]] StructuredBuffer<VirtualClipmap> clipmaps;
[[vk::binding(1, 2)]] Texture2DArray<u32> vsm_page_table;
[[vk::binding(2, 2)]] Texture2D<f32> vsm_physical_pages;

// TODO: Make these tweakable
static const u32 PCSS_BLOCKER_SEARCH_SAMPLES = 16;
static const u32 PCSS_PCF_SAMPLES = 24;
static const f32 PCSS_BLOCKER_SEARCH_RADIUS = 0.1;
static const f32 PCSS_LIGHT_WIDTH = 0.002;
static const f32 PCSS_MAX_PCF_RADIUS = 0.1;
// clipmap 0 is the finest level, so up close its texels get magnified and need a filter floor
static const f32 PCSS_MIN_PCF_TEXELS = 1.5;
static const f32 PCSS_MIN_BLOCKER_SEARCH_TEXELS = 4.0;

static const f32 VSM_DEPTH_MISS = -1.0;
static const f32 VSM_VISIBILITY_MISS = -1.0;

static const f32 VSM_POINT_TEXEL_REACH = 0.5;
static const f32 VSM_BILINEAR_TEXEL_REACH = 1.0;

func hash2(f32x2 p) -> f32x2 {
    let d = f32x2(dot(p, f32x2(127.1, 311.7)), dot(p, f32x2(269.5, 183.3)));
    return fract(sin(d) * 43758.5453);
}

func get_clipmap_texel_size(in VSMContext vsm_ctx, i32 clipmap_index) -> f32 {
    return exp2(f32(clipmap_index)) * vsm_ctx.get_first_clipmap_texel_length();
}

func get_vsm_bias_norm(
    in VSMContext vsm_ctx,
    i32 clipmap_index,
    f32x3 flat_normal,
    f32 NoL,
    f32 texel_reach,
    f32 extra_bias
) -> f32 {
    let L = vsm_ctx.directional_light_dir;
    let sqrt2 = 1.41421356;
    let quantize = 2.0 / (1 << 23);
    let b = sqrt2 * get_clipmap_texel_size(vsm_ctx, clipmap_index) * texel_reach;
    let base_bias = quantize + b + (NoL < 0.99 ? b * length(cross(flat_normal, L)) / max(NoL, 0.1) : b);
    return (base_bias + extra_bias) / vsm_ctx.z_length;
}

func get_vsm_page_origin(in VSMContext vsm_ctx, i32 clipmap_index, i32x2 virt_texel) -> i32x2 {
    if (any(virt_texel < 0) || any(virt_texel >= i32(vsm_ctx.virtual_extent))) {
        return i32x2(INVALID_COORDS);
    }

    let page_coords = virt_texel / vsm_ctx.page_size;
    let wrapped_coords = vsm_ctx.page_coords_virtual_to_wrapped(page_coords, clipmaps[clipmap_index].page_offset);
    if (any(wrapped_coords == INVALID_COORDS)) {
        return i32x2(INVALID_COORDS);
    }

    let page = VSMPageMetadata(vsm_page_table.Load(i32x4(wrapped_coords, clipmap_index, 0)));
    if (!page.is_backed()) {
        return i32x2(INVALID_COORDS);
    }

    let physical_page_coords = i32x2(page.get_physical_coords(vsm_ctx.physcial_page_table_size, vsm_ctx.page_size));
    return physical_page_coords * vsm_ctx.page_size;
}

func load_vsm_texel(in VSMContext vsm_ctx, i32 clipmap_index, i32x2 virt_texel) -> f32 {
    let page_origin = get_vsm_page_origin(vsm_ctx, clipmap_index, virt_texel);
    if (any(page_origin == INVALID_COORDS)) {
        return VSM_DEPTH_MISS;
    }

    return vsm_physical_pages.Load(i32x3(page_origin + virt_texel % vsm_ctx.page_size, 0));
}

func sample_vsm_shadow_depth(
    in VSMContext vsm_ctx,
    i32 clipmap_index,
    f32x3 world_pos
) -> f32 {
    if (clipmap_index < 0 || clipmap_index >= vsm_ctx.clipmap_count) {
        return VSM_DEPTH_MISS;
    }

    let clipmap_info = clipmaps[clipmap_index].get_clipmap_info(world_pos);
    let virt_texel = clipmap_info.get_virt_coords(i32x2(i32(vsm_ctx.virtual_extent)));
    return load_vsm_texel(vsm_ctx, clipmap_index, virt_texel);
}

func sample_vsm_shadow_depth_with_fallback(
    in VSMContext vsm_ctx,
    i32 base_clipmap_index,
    f32x3 world_pos,
    out i32 hit_clipmap_index
) -> f32 {
    hit_clipmap_index = base_clipmap_index;
    let d0 = sample_vsm_shadow_depth(vsm_ctx, base_clipmap_index, world_pos);
    if (d0 != VSM_DEPTH_MISS) { return d0; }
    hit_clipmap_index = base_clipmap_index - 1;
    let d1 = sample_vsm_shadow_depth(vsm_ctx, base_clipmap_index - 1, world_pos);
    if (d1 != VSM_DEPTH_MISS) { return d1; }
    hit_clipmap_index = base_clipmap_index + 1;
    let d2 = sample_vsm_shadow_depth(vsm_ctx, base_clipmap_index + 1, world_pos);
    if (d2 != VSM_DEPTH_MISS) { return d2; }
    return VSM_DEPTH_MISS;
}

// the 2x2 footprint can straddle a page boundary
func sample_vsm_visibility_bilinear(
    in VSMContext vsm_ctx,
    i32 clipmap_index,
    f32x3 world_pos,
    f32 receiver_depth,
    f32 bias_norm
) -> f32 {
    if (clipmap_index < 0 || clipmap_index >= vsm_ctx.clipmap_count) {
        return VSM_VISIBILITY_MISS;
    }

    let clipmap_info = clipmaps[clipmap_index].get_clipmap_info(world_pos);
    let texel_f = clipmap_info.clip_uv * vsm_ctx.virtual_extent - 0.5;
    let base_texel = i32x2(floor(texel_f));
    let weight = texel_f - f32x2(base_texel);

    var depths = f32x4(VSM_DEPTH_MISS);
    let base_page_origin = get_vsm_page_origin(vsm_ctx, clipmap_index, base_texel);
    let in_page_texel = base_texel % vsm_ctx.page_size;
    let is_single_page = all(base_page_origin != INVALID_COORDS) && all(in_page_texel < vsm_ctx.page_size - 1);
    if (is_single_page) {
        let texel = base_page_origin + in_page_texel;
        depths.x = vsm_physical_pages.Load(i32x3(texel, 0));
        depths.y = vsm_physical_pages.Load(i32x3(texel + i32x2(1, 0), 0));
        depths.z = vsm_physical_pages.Load(i32x3(texel + i32x2(0, 1), 0));
        depths.w = vsm_physical_pages.Load(i32x3(texel + i32x2(1, 1), 0));
    } else {
        depths.x = load_vsm_texel(vsm_ctx, clipmap_index, base_texel);
        depths.y = load_vsm_texel(vsm_ctx, clipmap_index, base_texel + i32x2(1, 0));
        depths.z = load_vsm_texel(vsm_ctx, clipmap_index, base_texel + i32x2(0, 1));
        depths.w = load_vsm_texel(vsm_ctx, clipmap_index, base_texel + i32x2(1, 1));
    }

    if (any(depths == VSM_DEPTH_MISS)) {
        return VSM_VISIBILITY_MISS;
    }

    let lit = select(depths + bias_norm >= receiver_depth, f32x4(1.0), f32x4(0.0));
    return lerp(lerp(lit.x, lit.y, weight.x), lerp(lit.z, lit.w, weight.x), weight.y);
}

func sample_vsm_visibility_bilinear_with_fallback(
    in VSMContext vsm_ctx,
    i32 base_clipmap_index,
    f32x3 world_pos,
    f32 receiver_depth,
    f32x3 flat_normal,
    f32 NoL,
    f32 extra_bias
) -> f32 {
    let v0 = sample_vsm_visibility_bilinear(
        vsm_ctx, base_clipmap_index, world_pos, receiver_depth,
        get_vsm_bias_norm(vsm_ctx, base_clipmap_index, flat_normal, NoL, VSM_BILINEAR_TEXEL_REACH, extra_bias)
    );
    if (v0 != VSM_VISIBILITY_MISS) { return v0; }
    let v1 = sample_vsm_visibility_bilinear(
        vsm_ctx, base_clipmap_index - 1, world_pos, receiver_depth,
        get_vsm_bias_norm(vsm_ctx, base_clipmap_index - 1, flat_normal, NoL, VSM_BILINEAR_TEXEL_REACH, extra_bias)
    );
    if (v1 != VSM_VISIBILITY_MISS) { return v1; }
    let v2 = sample_vsm_visibility_bilinear(
        vsm_ctx, base_clipmap_index + 1, world_pos, receiver_depth,
        get_vsm_bias_norm(vsm_ctx, base_clipmap_index + 1, flat_normal, NoL, VSM_BILINEAR_TEXEL_REACH, extra_bias)
    );
    if (v2 != VSM_VISIBILITY_MISS) { return v2; }
    return VSM_VISIBILITY_MISS;
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

    let clipmap_texel_size = get_clipmap_texel_size(vsm_ctx, base_clipmap_index);
    let normal_offset_world = clipmap_texel_size * (1.0 + 2.0 * (1.0 - NoL));
    let offset_world_position = world_position + flat_normal * normal_offset_world;

    var tangent = f32x3(0.0);
    var bitangent = f32x3(0.0);
    perpendicular_basis(L, tangent, bitangent);

    let receiver_info = clipmaps[base_clipmap_index].get_clipmap_info(offset_world_position);
    let d_receiver_norm = receiver_info.projected_depth;
    let d_receiver_world = d_receiver_norm * vsm_ctx.z_length;

    let hard_shadow_visibility = sample_vsm_visibility_bilinear_with_fallback(
        vsm_ctx, base_clipmap_index, offset_world_position, d_receiver_norm, flat_normal, NoL, 0.0
    );
    let hard_shadow = hard_shadow_visibility == VSM_VISIBILITY_MISS ? 1.0 : hard_shadow_visibility;

    // Blocker search
    let blocker_search_radius = max(PCSS_BLOCKER_SEARCH_RADIUS, PCSS_MIN_BLOCKER_SEARCH_TEXELS * clipmap_texel_size);
    var accum_depth_world = 0.0;
    var blockers = 0u;
    var valid_blocker_samples = 0u;
    for (var i = 0u; i < PCSS_BLOCKER_SEARCH_SAMPLES; i += 1u) {
        let xi = fract(com::hammersley2d(i, PCSS_BLOCKER_SEARCH_SAMPLES) + noise);
        let r = sqrt(xi.x) * blocker_search_radius;
        let theta = xi.y * TAU;
        let offset = r * (tangent * cos(theta) + bitangent * sin(theta));

        var hit_clipmap_index = base_clipmap_index;
        let depth = sample_vsm_shadow_depth_with_fallback(
            vsm_ctx, base_clipmap_index, offset_world_position + offset, hit_clipmap_index
        );
        if (depth == VSM_DEPTH_MISS) {
            continue;
        }

        valid_blocker_samples += 1u;

        let pcf_bias = 2.0 * r;
        let bias_norm = get_vsm_bias_norm(
            vsm_ctx, hit_clipmap_index, flat_normal, NoL, VSM_POINT_TEXEL_REACH, lerp(pcf_bias, 0.0, NoL)
        );
        if (depth + bias_norm < d_receiver_norm) {
            accum_depth_world += depth * vsm_ctx.z_length;
            blockers += 1u;
        }
    }

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
    let penumbra_radius = min(PCSS_MAX_PCF_RADIUS, (d_receiver_world - d_blocker_world) * PCSS_LIGHT_WIDTH);
    let pcf_radius = max(penumbra_radius, PCSS_MIN_PCF_TEXELS * clipmap_texel_size);

    // PCF
    var light_visibility = 0.0;
    var valid_pcf_samples = 0u;
    for (var i = 0u; i < PCSS_PCF_SAMPLES; i += 1u) {
        let xi = fract(com::hammersley2d(i, PCSS_PCF_SAMPLES) + noise.yx);
        let r = sqrt(xi.x) * pcf_radius;
        let theta = xi.y * TAU;
        let offset = r * (tangent * cos(theta) + bitangent * sin(theta));

        let pcf_bias = 2.0 * r;
        let visibility = sample_vsm_visibility_bilinear_with_fallback(
            vsm_ctx, base_clipmap_index, offset_world_position + offset, d_receiver_norm, flat_normal, NoL,
            lerp(pcf_bias, 0.0, NoL)
        );
        if (visibility == VSM_VISIBILITY_MISS) {
            continue;
        }

        valid_pcf_samples += 1u;
        light_visibility += visibility;
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

    let noise = hash2(f32x2(input.position.xy));
    let clipmap_selection = vsm_ctx.get_clipmap_selection(camera, input.tex_coord, depth, flat_N);
    let fine_visibility = pcss_shadow(
        vsm_ctx,
        i32(clipmap_selection.fine_clipmap_index),
        world_position,
        flat_N,
        noise
    );
    if (clipmap_selection.fine_clipmap_index == clipmap_selection.coarse_clipmap_index) {
        return fine_visibility;
    }

    let coarse_visibility = pcss_shadow(
        vsm_ctx,
        i32(clipmap_selection.coarse_clipmap_index),
        world_position,
        flat_N,
        noise
    );
    return lerp(fine_visibility, coarse_visibility, clipmap_selection.blend_weight);
}
