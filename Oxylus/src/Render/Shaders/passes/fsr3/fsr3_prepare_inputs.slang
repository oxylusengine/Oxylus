import common;
import fsr3;

struct ShaderParameters {
    ConstantBuffer<FSR3Constants> constants;

    Texture2D<f32x2> input_motion_vectors;
    Texture2D<f32> input_depth;
    Texture2D<f32x4> input_color;

    RWTexture2D<f32x2> dilated_motion_vectors;
    RWTexture2D<f32> dilated_depth;
    RWTexture2D<u32> reconstructed_prev_nearest_depth;
    RWTexture2D<f32> farthest_depth;
    RWTexture2D<f32> current_luma;
};
ParameterBlock<ShaderParameters> params;

struct DepthExtents {
    f32 nearest;
    i32x2 nearest_coord;
    f32 farthest;
};

// reversed-z, so "nearest" is the largest depth value
func find_depth_extents(i32x2 px_pos) -> DepthExtents {
    const i32x2 sample_offsets[9] = {
        i32x2(+0, +0), i32x2(+1, +0), i32x2(+0, +1),
        i32x2(+0, -1), i32x2(-1, +0), i32x2(-1, +1),
        i32x2(+1, +1), i32x2(-1, -1), i32x2(+1, -1),
    };

    // pulled out of the compare loop so the compiler can batch the loads
    f32 depth[9];
    [ForceUnroll]
    for (i32 i = 0; i < 9; i++) {
        let pos = px_pos + sample_offsets[i];
        depth[i] = params.input_depth.Load(i32x3(pos, 0));
    }

    DepthExtents extents;
    extents.nearest_coord = px_pos;
    extents.nearest = depth[0];
    extents.farthest = depth[0];

    [ForceUnroll]
    for (i32 i = 1; i < 9; i++) {
        let pos = px_pos + sample_offsets[i];
        if (fsr3_is_on_screen(pos, params.constants.render_size)) {
            let nd_depth = depth[i];
            if (nd_depth > extents.nearest) {
                extents.farthest = min(extents.farthest, nd_depth);
                extents.nearest_coord = pos;
                extents.nearest = nd_depth;
            }
        }
    }

    return extents;
}

// scatters this pixel's depth into the previous frame's grid, so the reactivity pass can tell a
// disocclusion from a surface that simply moved
func reconstruct_prev_depth(i32x2 px_pos, f32 depth, f32x2 motion_vector) -> void {
    let nearest_depth_in_meters = min(params.constants.get_view_space_depth_in_meters(depth), FSR3_FP16_MAX);
    let mv_threshold = fsr3_reconstructed_depth_mv_px_threshold(nearest_depth_in_meters);

    // discard small mvs
    let mv = motion_vector * f32(fsr3_get_4k_velocity(motion_vector) > mv_threshold);

    let uv = (f32x2(px_pos) + 0.5) / f32x2(params.constants.render_size);
    let reprojected_uv = uv + mv;
    let bilinear_info = fsr3_get_bilinear_sampling_data(reprojected_uv, params.constants.render_size);

    // push to every pixel the bilinear footprint touches
    [ForceUnroll]
    for (i32 i = 0; i < 4; i++) {
        if (bilinear_info.weights[i] > RECONSTRUCTED_DEPTH_BILINEAR_WEIGHT_THRESHOLD) {
            let store_pos = bilinear_info.base_pos + bilinear_info.offsets[i];
            if (fsr3_is_on_screen(store_pos, params.constants.render_size)) {
                var previous = 0u;
                InterlockedMax(params.reconstructed_prev_nearest_depth[u32x2(store_pos)], asuint(depth), previous);
            }
        }
    }
}

[[numthreads(8, 8, 1)]]
func cs_main(u32x3 thread_id : SV_DispatchThreadID) -> void {
    let px_pos = i32x2(thread_id.xy);

    let extents = find_depth_extents(px_pos);
    // render resolution motion vectors, so the dilated fetch is a direct load
    let dilated_motion_vector = params.input_motion_vectors.Load(i32x3(extents.nearest_coord, 0)) *
                                params.constants.motion_vector_scale;

    reconstruct_prev_depth(px_pos, extents.nearest, dilated_motion_vector);

    params.dilated_motion_vectors[thread_id.xy] = dilated_motion_vector;
    params.dilated_depth[thread_id.xy] = extents.nearest;

    let farthest_depth_in_meters = min(
        params.constants.get_view_space_depth_in_meters(extents.farthest),
        FSR3_FP16_MAX
    );
    params.farthest_depth[thread_id.xy] = farthest_depth_in_meters;

    // linear input, so luma needs no transfer function round trip
    let rgb = max(f32x3(0.0), params.input_color.Load(i32x3(px_pos, 0)).rgb);
    params.current_luma[thread_id.xy] = fsr3_rgb_to_luma(rgb);
}
