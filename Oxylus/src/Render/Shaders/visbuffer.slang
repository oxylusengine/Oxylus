module visbuffer;

import common;
import scene;

constexpr static u64 MESHLET_DEPTH_BITS = 32u;
constexpr static u64 MESHLET_DEPTH_MASK = (1u << MESHLET_DEPTH_BITS) - 1u;

constexpr static u32 MESHLET_INSTANCE_ID_BITS = 24u;
constexpr static u32 MESHLET_INSTANCE_ID_MASK = (1u << MESHLET_INSTANCE_ID_BITS) - 1u;
// We can support up to 256 triangles per meshlet.
// Do not ever go higher than that.
public constexpr static u32 MESHLET_PRIMITIVE_BITS = 8u;
public constexpr static u32 MESHLET_PRIMITIVE_MASK = (1u << MESHLET_PRIMITIVE_BITS) - 1u;

public constexpr static u32 TERRAIN_INSTANCE_ID = MESHLET_INSTANCE_ID_MASK - 1u;

public func is_terrain(u32 data) -> bool {
    return (data >> MESHLET_PRIMITIVE_BITS) == TERRAIN_INSTANCE_ID;
}

public struct VisBufferData {
    public u32 meshlet_instance_index;
    public u32 triangle_index;

    [[mutating]]
    public __init(u32 meshlet_instance_index, u32 triangle_index) {
        this.meshlet_instance_index = meshlet_instance_index;
        this.triangle_index = triangle_index;
    }

    [[mutating]]
    public __init(u32 data) {
        this.meshlet_instance_index = (data >> MESHLET_PRIMITIVE_BITS) & MESHLET_INSTANCE_ID_MASK;
        this.triangle_index = data & MESHLET_PRIMITIVE_MASK;
    }

    public func encode() -> u32 {
        return (this.meshlet_instance_index << MESHLET_PRIMITIVE_BITS) | (this.triangle_index & MESHLET_PRIMITIVE_MASK);
    }
};

// NOTE: This version of visibility buffering is not widely available.
// My current idea is to have 2 (R32, D32) attachments and render them
// just normally, and then have a compute pass to merge them into R64
// image. R64 images are not widely available even with storage only.
// Investigate maintenance8 for D32<->R32 copies.

public struct VisBuffer {
    u32 data;
    public f32 depth;

    [[mutating]]
    public __init(u32 meshlet_instance_index, u32 triangle_index, f32 depth) {
        let data = VisBufferData(meshlet_instance_index, triangle_index);
        this.data = data.encode();
        this.depth = depth;
    }

    [[mutating]]
    public __init(in VisBufferData data, f32 depth) {
        this.data = data.encode();
        this.depth = depth;
    }

    [[mutating]]
    public __init(u64 data) {
        this.data = u32(data & MESHLET_DEPTH_MASK);
        this.depth = asfloat(u32(data >> MESHLET_DEPTH_BITS));
    }

    public func encode() -> u64 {
        return (u64(asuint(this.depth)) << MESHLET_DEPTH_BITS) | u64(this.data);
    }

    public func decode_vis() -> VisBufferData {
        return VisBufferData(this.data);
    };
};

public struct PartialDeriv {
    public f32x3 ddx;
    public f32x3 ddy;
    public f32x3 lambda;
    public bool is_back_facing;

    public func gradient_of(f32x2x3 v) -> UVGradient {
        UVGradient grad;
        grad.uv = mul(this.lambda, v);
        grad.ddx = mul(this.ddx, v);
        grad.ddy = mul(this.ddy, v);

        return grad;
    }
};

public func compute_partial_derivatives(
    in f32x4x3 world_positions, in f32x2 uv, in f32x2 resolution, in f32x4x4 projection_view
) -> PartialDeriv {
    PartialDeriv result;

    let clip_pos_0 = mul(projection_view, f32x4(world_positions[0].xyz, 1.0));
    let clip_pos_1 = mul(projection_view, f32x4(world_positions[1].xyz, 1.0));
    let clip_pos_2 = mul(projection_view, f32x4(world_positions[2].xyz, 1.0));

    // Partial Derivatives
    let inv_w = 1.0 / f32x3(clip_pos_0.w, clip_pos_1.w, clip_pos_2.w);
    let ndc_0 = clip_pos_0.xy * inv_w[0];
    let ndc_1 = clip_pos_1.xy * inv_w[1];
    let ndc_2 = clip_pos_2.xy * inv_w[2];
    // Inverse area of a triangle.
    // https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf
    // Appendix A:
    // D =
    let det = determinant(f32x2x2(ndc_2 - ndc_1, ndc_0 - ndc_1));
    // the 3x3 clip determinant test_triangle_backface uses, scaled by 1/(w0*w1*w2), which turns negative when a
    // vertex is behind the camera, as on a large floor or counter triangle passing under the eye
    let is_w_product_negative = (clip_pos_0.w < 0.0) != (clip_pos_1.w < 0.0) != (clip_pos_2.w < 0.0);
    result.is_back_facing = (det > 0.0) != is_w_product_negative;
    let inv_det = 1.0 / det;
    // Lambda 1 =
    result.ddx = f32x3(ndc_1.y - ndc_2.y, ndc_2.y - ndc_0.y, ndc_0.y - ndc_1.y) * inv_det * inv_w;
    // Lambda 2 =
    result.ddy = f32x3(ndc_2.x - ndc_1.x, ndc_0.x - ndc_2.x, ndc_1.x - ndc_0.x) * inv_det * inv_w;
    var ddx_sum = dot(result.ddx, 1.0);
    var ddy_sum = dot(result.ddy, 1.0);

    let delta_v = uv - ndc_0;
    let interp_inv_w = inv_w.x + delta_v.x * ddx_sum + delta_v.y * ddy_sum;
    let interp_w = 1.0 / interp_inv_w;
    result.lambda = f32x3(
        interp_w * (inv_w[0] + delta_v.x * result.ddx.x + delta_v.y * result.ddy.x),
        interp_w * (delta_v.x * result.ddx.y + delta_v.y * result.ddy.y),
        interp_w * (delta_v.x * result.ddx.z + delta_v.y * result.ddy.z)
    );
    let two_over_resolution = 2.0 / resolution;
    result.ddx *= two_over_resolution.x;
    result.ddy *= -two_over_resolution.y;
    ddx_sum *= two_over_resolution.x;
    ddy_sum *= -two_over_resolution.y;

    let interp_ddx_w = 1.0 / (interp_inv_w + ddx_sum);
    let interp_ddy_w = 1.0 / (interp_inv_w + ddy_sum);
    result.ddx = interp_ddx_w * (result.lambda * interp_inv_w + result.ddx) - result.lambda;
    result.ddy = interp_ddy_w * (result.lambda * interp_inv_w + result.ddy) - result.lambda;

    return result;
}
