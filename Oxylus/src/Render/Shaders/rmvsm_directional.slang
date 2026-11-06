import common;
import gpu;
import scene;
import rmvsm;

struct VSMDirectionalResources {
    VirtualClipmap *clipmaps;
    Texture2DArray<u32> page_table;
    Texture2D<f32> physical_pages;
    SamplerState sampler;
};

struct VSMCenterPage {
    i32 clipmap_index;
    i32x2 page_coords;
    i32x2 origin;
};

struct VSMReceiver {
    f32x2 light_xy;
    f32 depth;
    f32 slope;
    f32x2 base_uv;
    f32 base_uv_scale;
    VSMCenterPage center;
};

// TODO: Make these tweakable
[[vk::constant_id(55)]] const i32 VSM_DIRECTIONAL_PAGE_TABLE_SIZE = 64;

static const i32 VSM_VIRTUAL_EXTENT = VSM_PAGE_SIZE * VSM_DIRECTIONAL_PAGE_TABLE_SIZE;
static const i32 VSM_PHYSICAL_PAGES_PER_DIM = VSM_PHYSICAL_PAGE_TABLE_SIZE / VSM_PAGE_SIZE;

static const u32 VSM_FETCH_GROUP = 2;
static const u32 PCSS_BLOCKER_SEARCH_SAMPLES = 16;
static const u32 PCSS_PCF_SAMPLES = 24;
static const u32 PCSS_TEMPORAL_BLOCKER_SEARCH_SAMPLES = 8;
static const u32 PCSS_TEMPORAL_PCF_SAMPLES = 12;
static const f32 VOGEL_GOLDEN_ANGLE = 2.39996323;
static const f32 PCSS_BLOCKER_SEARCH_RADIUS = 0.1;
static const f32 PCSS_LIGHT_WIDTH = 0.002;
static const f32 PCSS_MAX_PCF_RADIUS = 0.1;
// clipmap 0 is the finest level, so up close its texels get magnified and need a filter floor
static const f32 PCSS_MIN_PCF_TEXELS = 1.5;
static const f32 PCSS_MIN_BLOCKER_SEARCH_TEXELS = 4.0;

static const f32 VOGEL_RADII_8[8] = { 0.25, 0.433012694, 0.559017003, 0.661437809, 0.75, 0.82915622, 0.901387811, 0.968245864 };
static const f32 VOGEL_RADII_12[12] = { 0.204124153, 0.353553385, 0.456435472, 0.540061712, 0.612372458, 0.677003205, 0.735980093, 0.790569425, 0.841625392, 0.889756501, 0.935414374, 0.978945017 };
static const f32 VOGEL_RADII_16[16] = { 0.176776692, 0.306186229, 0.395284712, 0.467707187, 0.530330062, 0.586301982, 0.637377441, 0.684653223, 0.728868961, 0.770551741, 0.810092568, 0.847791255, 0.883883476, 0.918558657, 0.95197165, 0.984250963 };
static const f32 VOGEL_RADII_24[24] = { 0.144337565, 0.25, 0.322748601, 0.381881297, 0.433012694, 0.478713542, 0.520416498, 0.559017003, 0.595119059, 0.629152894, 0.661437809, 0.692218661, 0.721687853, 0.75, 0.777281582, 0.803637564, 0.82915622, 0.853912592, 0.877971172, 0.901387811, 0.924211383, 0.946484745, 0.968245864, 0.989528537 };

static const f32 VSM_DEPTH_MISS = -1.0;
static const f32 VSM_VISIBILITY_MISS = -1.0;

static const f32 VSM_POINT_TEXEL_REACH = 0.5;
static const f32 VSM_BILINEAR_TEXEL_REACH = 1.0;

static const i32 VSM_FALLBACK_CLIPMAP_OFFSETS[3] = { 0, -1, 1 };

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
    f32 slope,
    f32 texel_reach,
    f32 extra_bias
) -> f32 {
    let sqrt2 = 1.41421356;
    let quantize = 2.0 / (1 << 23);
    let b = sqrt2 * get_clipmap_texel_size(vsm_ctx, clipmap_index) * texel_reach;
    return (quantize + b * (1.0 + slope) + extra_bias) / vsm_ctx.z_length;
}

func get_vsm_slope(in VSMContext vsm_ctx, f32x3 flat_normal, f32 NoL) -> f32 {
    return NoL < 0.99 ? length(cross(flat_normal, vsm_ctx.directional_light_dir)) / max(NoL, 0.1) : 1.0;
}

func vogel_radius_unit(u32 k, u32 count) -> f32 {
    if (count == 8u) {
        return VOGEL_RADII_8[k];
    }

    if (count == 12u) {
        return VOGEL_RADII_12[k];
    }

    if (count == 16u) {
        return VOGEL_RADII_16[k];
    }

    return VOGEL_RADII_24[k];
}

func get_vsm_level_uv(in VSMContext vsm_ctx, in VSMDirectionalResources res, i32 clipmap_index, f32x2 light_xy, f32x2 offset_meters) -> f32x2 {
    let scale = exp2(-f32(clipmap_index));
    let shift = 2.0 * f32x2(res.clipmaps[clipmap_index].page_offset) / f32(VSM_DIRECTIONAL_PAGE_TABLE_SIZE);
    return (light_xy * scale - shift) * 0.5 + 0.5 + offset_meters * (0.5 * scale / vsm_ctx.first_clipmap_width);
}

func get_receiver_uv(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMReceiver receiver,
    i32 base_clipmap_index,
    i32 clipmap_index,
    f32x2 offset_meters
) -> f32x2 {
    if (clipmap_index == base_clipmap_index) {
        return receiver.base_uv + offset_meters * receiver.base_uv_scale;
    }

    return get_vsm_level_uv(vsm_ctx, res, clipmap_index, receiver.light_xy, offset_meters);
}

func lookup_vsm_page_origin(
    in VSMDirectionalResources res,
    i32 clipmap_index,
    i32x2 page_coords
) -> i32x2 {
    let wrapped_coords = (page_coords + res.clipmaps[clipmap_index].page_offset) % VSM_DIRECTIONAL_PAGE_TABLE_SIZE;
    let non_negative_coords = (wrapped_coords + VSM_DIRECTIONAL_PAGE_TABLE_SIZE) % VSM_DIRECTIONAL_PAGE_TABLE_SIZE;

    let page = VSMPageMetadata(res.page_table.Load(i32x4(non_negative_coords, clipmap_index, 0)));
    if (!page.is_backed()) {
        return i32x2(INVALID_COORDS);
    }

    let address = i32(page.get_physical_address());
    let physical_page_coords = i32x2(address % VSM_PHYSICAL_PAGES_PER_DIM, address / VSM_PHYSICAL_PAGES_PER_DIM);
    return physical_page_coords * VSM_PAGE_SIZE;
}

func get_vsm_page_origin(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMCenterPage center,
    i32 clipmap_index,
    i32x2 virt_texel
) -> i32x2 {
    if (any(virt_texel < 0) || any(virt_texel >= VSM_VIRTUAL_EXTENT)) {
        return i32x2(INVALID_COORDS);
    }

    let page_coords = virt_texel / VSM_PAGE_SIZE;
    if (clipmap_index == center.clipmap_index && all(page_coords == center.page_coords)) {
        return center.origin;
    }

    return lookup_vsm_page_origin(res, clipmap_index, page_coords);
}

func make_vsm_center_page(in VSMDirectionalResources res, i32 clipmap_index, f32x2 uv) -> VSMCenterPage {
    var center = VSMCenterPage(clipmap_index, i32x2(INVALID_COORDS), i32x2(INVALID_COORDS));
    if (any(uv < 0.0) || any(uv > 1.0)) {
        return center;
    }

    let virt_texel = i32x2(floor(uv * f32(VSM_VIRTUAL_EXTENT)));
    if (any(virt_texel >= VSM_VIRTUAL_EXTENT)) {
        return center;
    }

    center.page_coords = virt_texel / VSM_PAGE_SIZE;
    center.origin = lookup_vsm_page_origin(res, clipmap_index, center.page_coords);
    return center;
}

func load_vsm_texel(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMCenterPage center,
    i32 clipmap_index,
    i32x2 virt_texel
) -> f32 {
    let page_origin = get_vsm_page_origin(vsm_ctx, res, center, clipmap_index, virt_texel);
    if (any(page_origin == INVALID_COORDS)) {
        return VSM_DEPTH_MISS;
    }

    return res.physical_pages.Load(i32x3(page_origin + virt_texel % VSM_PAGE_SIZE, 0));
}

func sample_vsm_shadow_depth(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMReceiver receiver,
    i32 base_clipmap_index,
    i32 clipmap_index,
    f32x2 offset_meters
) -> f32 {
    if (clipmap_index < 0 || clipmap_index >= vsm_ctx.clipmap_count) {
        return VSM_DEPTH_MISS;
    }

    let uv = get_receiver_uv(vsm_ctx, res, receiver, base_clipmap_index, clipmap_index, offset_meters);
    if (any(uv < 0.0) || any(uv > 1.0)) {
        return VSM_DEPTH_MISS;
    }

    let virt_texel = i32x2(floor(uv * f32(VSM_VIRTUAL_EXTENT)));
    return load_vsm_texel(vsm_ctx, res, receiver.center, clipmap_index, virt_texel);
}

func sample_vsm_shadow_depth_with_fallback(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMReceiver receiver,
    i32 base_clipmap_index,
    f32x2 offset_meters,
    out i32 hit_clipmap_index
) -> f32 {
    [loop]
    for (var i = 0; i < 3; i++) {
        hit_clipmap_index = base_clipmap_index + VSM_FALLBACK_CLIPMAP_OFFSETS[i];
        let depth = sample_vsm_shadow_depth(
            vsm_ctx, res, receiver, base_clipmap_index, hit_clipmap_index, offset_meters
        );
        if (depth != VSM_DEPTH_MISS) {
            return depth;
        }
    }

    return VSM_DEPTH_MISS;
}

func filter_vsm_depths(f32x4 depths, f32 receiver_depth, f32 bias_norm, f32x2 weight) -> f32 {
    let lit = select(depths + bias_norm >= receiver_depth, f32x4(1.0), f32x4(0.0));
    return lerp(lerp(lit.x, lit.y, weight.x), lerp(lit.z, lit.w, weight.x), weight.y);
}

// the 2x2 footprint can straddle a page boundary
func sample_vsm_visibility_bilinear(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMCenterPage center,
    i32 clipmap_index,
    f32x2 uv,
    f32 receiver_depth,
    f32 bias_norm
) -> f32 {
    if (clipmap_index < 0 || clipmap_index >= vsm_ctx.clipmap_count) {
        return VSM_VISIBILITY_MISS;
    }

    let texel_f = uv * f32(VSM_VIRTUAL_EXTENT) - 0.5;
    let base_texel = i32x2(floor(texel_f));
    let weight = texel_f - f32x2(base_texel);

    var depths = f32x4(VSM_DEPTH_MISS);
    let base_page_origin = get_vsm_page_origin(vsm_ctx, res, center, clipmap_index, base_texel);
    let in_page_texel = base_texel % VSM_PAGE_SIZE;
    let is_single_page = all(base_page_origin != INVALID_COORDS) && all(in_page_texel < VSM_PAGE_SIZE - 1);
    if (is_single_page) {
        let texel = base_page_origin + in_page_texel;
        let gathered = res.physical_pages.Gather(
            res.sampler, (f32x2(texel) + 1.0) / f32(VSM_PHYSICAL_PAGE_TABLE_SIZE)
        );
        depths = gathered.wzxy;
    } else {
        depths.x = load_vsm_texel(vsm_ctx, res, center, clipmap_index, base_texel);
        depths.y = load_vsm_texel(vsm_ctx, res, center, clipmap_index, base_texel + i32x2(1, 0));
        depths.z = load_vsm_texel(vsm_ctx, res, center, clipmap_index, base_texel + i32x2(0, 1));
        depths.w = load_vsm_texel(vsm_ctx, res, center, clipmap_index, base_texel + i32x2(1, 1));
    }

    if (any(depths == VSM_DEPTH_MISS)) {
        return VSM_VISIBILITY_MISS;
    }

    return filter_vsm_depths(depths, receiver_depth, bias_norm, weight);
}

func sample_vsm_visibility_bilinear_with_fallback(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMReceiver receiver,
    i32 base_clipmap_index,
    f32x2 offset_meters,
    f32 extra_bias
) -> f32 {
    [loop]
    for (var i = 0; i < 3; i++) {
        let clipmap_index = base_clipmap_index + VSM_FALLBACK_CLIPMAP_OFFSETS[i];
        if (clipmap_index < 0 || clipmap_index >= vsm_ctx.clipmap_count) {
            continue;
        }

        let uv = get_receiver_uv(vsm_ctx, res, receiver, base_clipmap_index, clipmap_index, offset_meters);
        let bias_norm = get_vsm_bias_norm(vsm_ctx, clipmap_index, receiver.slope, VSM_BILINEAR_TEXEL_REACH, extra_bias);
        let visibility = sample_vsm_visibility_bilinear(
            vsm_ctx, res, receiver.center, clipmap_index, uv, receiver.depth, bias_norm
        );
        if (visibility != VSM_VISIBILITY_MISS) {
            return visibility;
        }
    }

    return VSM_VISIBILITY_MISS;
}

func pcss_hard_shadow(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMReceiver receiver,
    i32 base_clipmap_index
) -> f32 {
    let visibility = sample_vsm_visibility_bilinear_with_fallback(
        vsm_ctx, res, receiver, base_clipmap_index, f32x2(0.0), 0.0
    );
    return visibility == VSM_VISIBILITY_MISS ? 1.0 : visibility;
}

func pcss_soft_shadow(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
    in VSMReceiver receiver,
    i32 base_clipmap_index,
    f32 NoL,
    f32x2 noise
) -> f32 {
    let is_temporal = (scene_flags & SceneFlags::HasTemporalUpscaler) != 0;
    let blocker_sample_count = is_temporal ? PCSS_TEMPORAL_BLOCKER_SEARCH_SAMPLES : PCSS_BLOCKER_SEARCH_SAMPLES;
    let pcf_sample_count = is_temporal ? PCSS_TEMPORAL_PCF_SAMPLES : PCSS_PCF_SAMPLES;

    let clipmap_texel_size = get_clipmap_texel_size(vsm_ctx, base_clipmap_index);
    let d_receiver_world = receiver.depth * vsm_ctx.z_length;

    var golden_sin = 0.0;
    var golden_cos = 0.0;
    sincos(VOGEL_GOLDEN_ANGLE, golden_sin, golden_cos);

    // samples are fetched four at a time from the receiver's page so the loads issue back to back,
    // lanes that land elsewhere take the full lookup
    let is_center_valid = all(receiver.center.origin != INVALID_COORDS);
    let center_origin = is_center_valid ? receiver.center.origin : i32x2(0);
    let center_texel_min = receiver.center.page_coords * VSM_PAGE_SIZE;

    // blocker search
    let blocker_search_radius = max(PCSS_BLOCKER_SEARCH_RADIUS, PCSS_MIN_BLOCKER_SEARCH_TEXELS * clipmap_texel_size);
    var accum_depth_world = 0.0;
    var blockers = 0u;
    var valid_blocker_samples = 0u;
    var blocker_dir = f32x2(0.0);
    sincos(noise.x * TAU, blocker_dir.y, blocker_dir.x);
    [loop]
    for (var group = 0u; group < blocker_sample_count; group += VSM_FETCH_GROUP) {
        f32x2 offsets_meters[VSM_FETCH_GROUP];
        bool is_fast[VSM_FETCH_GROUP];
        f32 fetched[VSM_FETCH_GROUP];

        [unroll]
        for (var j = 0u; j < VSM_FETCH_GROUP; j += 1u) {
            offsets_meters[j] = blocker_dir * (vogel_radius_unit(group + j, blocker_sample_count) * blocker_search_radius);
            blocker_dir = f32x2(
                blocker_dir.x * golden_cos - blocker_dir.y * golden_sin,
                blocker_dir.x * golden_sin + blocker_dir.y * golden_cos
            );

            let uv = receiver.base_uv + offsets_meters[j] * receiver.base_uv_scale;
            let texel = i32x2(floor(uv * f32(VSM_VIRTUAL_EXTENT)));
            is_fast[j] = is_center_valid && all(uv >= 0.0) && all(uv <= 1.0) && all(texel < VSM_VIRTUAL_EXTENT) &&
                         all(texel / VSM_PAGE_SIZE == receiver.center.page_coords);
            let fetch_coords = center_origin + clamp(texel - center_texel_min, 0, VSM_PAGE_SIZE - 1);
            fetched[j] = res.physical_pages.Load(i32x3(fetch_coords, 0));
        }

        [unroll]
        for (var j = 0u; j < VSM_FETCH_GROUP; j += 1u) {
            var hit_clipmap_index = base_clipmap_index;
            var depth = fetched[j];
            if (!is_fast[j] || depth == VSM_DEPTH_MISS) {
                depth = sample_vsm_shadow_depth_with_fallback(
                    vsm_ctx, res, receiver, base_clipmap_index, offsets_meters[j], hit_clipmap_index
                );
            }

            if (depth == VSM_DEPTH_MISS) {
                continue;
            }

            valid_blocker_samples += 1u;

            let pcf_bias = 2.0 * vogel_radius_unit(group + j, blocker_sample_count) * blocker_search_radius;
            let bias_norm = get_vsm_bias_norm(
                vsm_ctx, hit_clipmap_index, receiver.slope, VSM_POINT_TEXEL_REACH, lerp(pcf_bias, 0.0, NoL)
            );
            if (depth + bias_norm < receiver.depth) {
                accum_depth_world += depth * vsm_ctx.z_length;
                blockers += 1u;
            }
        }
    }

    if (valid_blocker_samples == 0u) {
        return VSM_VISIBILITY_MISS;
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

    // pcf
    var light_visibility = 0.0;
    var valid_pcf_samples = 0u;
    var pcf_dir = f32x2(0.0);
    sincos(noise.y * TAU, pcf_dir.y, pcf_dir.x);
    [loop]
    for (var group = 0u; group < pcf_sample_count; group += VSM_FETCH_GROUP) {
        f32x2 offsets_meters[VSM_FETCH_GROUP];
        bool is_fast[VSM_FETCH_GROUP];
        f32x4 gathered[VSM_FETCH_GROUP];

        [unroll]
        for (var j = 0u; j < VSM_FETCH_GROUP; j += 1u) {
            let r = vogel_radius_unit(group + j, pcf_sample_count) * pcf_radius;
            offsets_meters[j] = pcf_dir * r;
            pcf_dir = f32x2(
                pcf_dir.x * golden_cos - pcf_dir.y * golden_sin,
                pcf_dir.x * golden_sin + pcf_dir.y * golden_cos
            );

            let uv = receiver.base_uv + offsets_meters[j] * receiver.base_uv_scale;
            let texel_f = uv * f32(VSM_VIRTUAL_EXTENT) - 0.5;
            let base_texel = i32x2(floor(texel_f));
            let in_page_texel = base_texel - center_texel_min;
            is_fast[j] = is_center_valid && all(base_texel >= 0) && all(base_texel < VSM_VIRTUAL_EXTENT) &&
                         all(base_texel / VSM_PAGE_SIZE == receiver.center.page_coords) &&
                         all(in_page_texel < VSM_PAGE_SIZE - 1);
            let fetch_texel = center_origin + clamp(in_page_texel, 0, VSM_PAGE_SIZE - 2);
            gathered[j] = res.physical_pages.Gather(
                res.sampler, (f32x2(fetch_texel) + 1.0) / f32(VSM_PHYSICAL_PAGE_TABLE_SIZE)
            );
        }

        [unroll]
        for (var j = 0u; j < VSM_FETCH_GROUP; j += 1u) {
            let extra_bias = lerp(2.0 * vogel_radius_unit(group + j, pcf_sample_count) * pcf_radius, 0.0, NoL);
            var visibility = VSM_VISIBILITY_MISS;
            let depths = gathered[j].wzxy;
            if (is_fast[j] && !any(depths == VSM_DEPTH_MISS)) {
                let uv = receiver.base_uv + offsets_meters[j] * receiver.base_uv_scale;
                let texel_f = uv * f32(VSM_VIRTUAL_EXTENT) - 0.5;
                let bias_norm = get_vsm_bias_norm(
                    vsm_ctx, base_clipmap_index, receiver.slope, VSM_BILINEAR_TEXEL_REACH, extra_bias
                );
                visibility = filter_vsm_depths(depths, receiver.depth, bias_norm, texel_f - floor(texel_f));
            } else {
                visibility = sample_vsm_visibility_bilinear_with_fallback(
                    vsm_ctx, res, receiver, base_clipmap_index, offsets_meters[j], extra_bias
                );
            }

            if (visibility == VSM_VISIBILITY_MISS) {
                continue;
            }

            valid_pcf_samples += 1u;
            light_visibility += visibility;
        }
    }

    if (valid_pcf_samples == 0u) {
        return VSM_VISIBILITY_MISS;
    }

    return light_visibility / f32(valid_pcf_samples);
}

func pcss_shadow(
    in VSMContext vsm_ctx,
    in VSMDirectionalResources res,
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

    let ndc0 = mul(res.clipmaps[0].projection_view_mat, f32x4(offset_world_position, 1.0)).xyz;
    let shift0 = 2.0 * f32x2(res.clipmaps[0].page_offset) / f32(VSM_DIRECTIONAL_PAGE_TABLE_SIZE);

    var receiver : VSMReceiver;
    receiver.light_xy = ndc0.xy + shift0;
    receiver.depth = ndc0.z;
    receiver.slope = get_vsm_slope(vsm_ctx, flat_normal, NoL);
    receiver.base_uv = get_vsm_level_uv(vsm_ctx, res, base_clipmap_index, receiver.light_xy, f32x2(0.0));
    receiver.base_uv_scale = 0.5 * exp2(-f32(base_clipmap_index)) / vsm_ctx.first_clipmap_width;
    receiver.center = make_vsm_center_page(res, base_clipmap_index, receiver.base_uv);

    let soft_visibility = pcss_soft_shadow(vsm_ctx, res, receiver, base_clipmap_index, NoL, noise);
    if (soft_visibility != VSM_VISIBILITY_MISS) {
        return soft_visibility;
    }

    return pcss_hard_shadow(vsm_ctx, res, receiver, base_clipmap_index);
}

func vsm_directional_visibility(
    in PBRLighting lighting,
    in VSMDirectionalResources res,
    in Camera camera,
    f32x2 uv,
    f32x2 pixel_coords,
    f32 depth,
    f32x3 flat_normal,
    f32x3 world_position
) -> f32 {
    let vsm_ctx = lighting.vsm;
    let clipmap_selection = vsm_ctx.get_clipmap_selection(camera, uv, depth, flat_normal);

    if ((scene_flags & SceneFlags::HasTemporalUpscaler) != 0) {
        let frame_coords = pixel_coords + f32(lighting.frame_index & 63u) * f32x2(17.0, 31.0);
        let noise = hash2(frame_coords);
        var clipmap_index = i32(clipmap_selection.fine_clipmap_index);
        if (clipmap_selection.fine_clipmap_index != clipmap_selection.coarse_clipmap_index) {
            let is_coarse = hash2(frame_coords.yx + 91.7).x < clipmap_selection.blend_weight;
            clipmap_index = is_coarse ? i32(clipmap_selection.coarse_clipmap_index) : clipmap_index;
        }

        return pcss_shadow(vsm_ctx, res, clipmap_index, world_position, flat_normal, noise);
    }

    let noise = hash2(pixel_coords);
    let fine_visibility = pcss_shadow(
        vsm_ctx,
        res,
        i32(clipmap_selection.fine_clipmap_index),
        world_position,
        flat_normal,
        noise
    );
    if (clipmap_selection.fine_clipmap_index == clipmap_selection.coarse_clipmap_index) {
        return fine_visibility;
    }

    let coarse_visibility = pcss_shadow(
        vsm_ctx,
        res,
        i32(clipmap_selection.coarse_clipmap_index),
        world_position,
        flat_normal,
        noise
    );
    return lerp(fine_visibility, coarse_visibility, clipmap_selection.blend_weight);
}
