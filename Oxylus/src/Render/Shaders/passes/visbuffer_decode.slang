import common;
import gpu;
import scene;
import visbuffer;
import hit;

#include <fullscreen.slang>

struct ShaderParameters {
    ConstantBuffer<Camera> camera;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<MeshInstance> mesh_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<TransformWorld> transforms;
    StructuredBuffer<TransformPrevious> transforms_previous;
    StructuredBuffer<Material> materials;

    Image2D<u32> visbuffer;
};
ParameterBlock<ShaderParameters> params;

struct FragmentOutput {
    f32x4 albedo_color : SV_Target0;
    f32x4 normal_color : SV_Target1;
    f32x3 emission_color : SV_Target2;
    f32x2 metallic_roughness_color : SV_Target3;
    f32x2 velocity : SV_Target4;
};

// screen space motion in render resolution pixels, pointing from this pixel back to where the
// surface was last frame. built from the unjittered matrices so the upscaler gets jitter free
// vectors, and oxylus ndc already runs +y down like texel space so neither axis is flipped
func compute_velocity(in TriangleHit tri, in f32x3 bary, in TransformPrevious transform_previous) -> f32x2 {
    const f32x3x3 previous_world_positions = {
        mul(transform_previous.previous_world, f32x4(tri.object_positions[0], 1.0)).xyz,
        mul(transform_previous.previous_world, f32x4(tri.object_positions[1], 1.0)).xyz,
        mul(transform_previous.previous_world, f32x4(tri.object_positions[2], 1.0)).xyz,
    };

    let current_clip = mul(params.camera.projection_view, f32x4(tri.interpolate_position(bary), 1.0));
    let previous_clip = mul(params.camera.previous_projection_view, f32x4(mul(bary, previous_world_positions), 1.0));
    if (current_clip.w <= 0.0 || previous_clip.w <= 0.0) {
        return f32x2(0.0);
    }

    let delta_ndc = (previous_clip.xy / previous_clip.w) - (current_clip.xy / current_clip.w);

    return delta_ndc * 0.5 * params.camera.resolution;
}

struct PartialDeriv {
    f32x3 ddx;
    f32x3 ddy;
    f32x3 lambda;

    func gradient_of(f32x2x3 v) -> UVGradient {
        UVGradient grad;
        grad.uv = mul(this.lambda, v);
        grad.ddx = mul(this.ddx, v);
        grad.ddy = mul(this.ddy, v);

        return grad;
    }
};

func compute_partial_derivatives(in f32x4x3 world_positions, in f32x2 uv, in f32x2 resolution) -> PartialDeriv {
    PartialDeriv result;

    let clip_pos_0 = mul(params.camera.projection_view, f32x4(world_positions[0].xyz, 1.0));
    let clip_pos_1 = mul(params.camera.projection_view, f32x4(world_positions[1].xyz, 1.0));
    let clip_pos_2 = mul(params.camera.projection_view, f32x4(world_positions[2].xyz, 1.0));

    // Partial Derivatives
    let inv_w = 1.0 / f32x3(clip_pos_0.w, clip_pos_1.w, clip_pos_2.w);
    let ndc_0 = clip_pos_0.xy * inv_w[0];
    let ndc_1 = clip_pos_1.xy * inv_w[1];
    let ndc_2 = clip_pos_2.xy * inv_w[2];
    // Inverse area of a triangle.
    // https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf
    // Appendix A:
    // D =
    let inv_det = 1.0 / determinant(f32x2x2(ndc_2 - ndc_1, ndc_0 - ndc_1));
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

[[shader("fragment")]]
func fs_main(VertexOutput input) -> FragmentOutput {
    let texel = params.visbuffer.Load(u32x3(u32x2(input.position.xy), 0));
    if (texel == ~0u || is_terrain(texel)) {
        discard;
    }

    FragmentOutput output = {};

    let geometry = SceneGeometry(params.mesh_instances, params.meshes, params.transforms, params.materials);
    let tri = resolve_visbuffer_triangle(geometry, params.meshlet_instances, VisBufferData(texel));
    if (!tri.valid) {
        return output;
    }

    let material = params.materials[tri.material_index];

    let NDC = f32x3(input.tex_coord * 2.0 - 1.0, 1.0);
    let deriv = compute_partial_derivatives(tri.world_positions, NDC.xy, params.camera.resolution);
    var tex_coord_grad = deriv.gradient_of(tri.tex_coords);
    material.transform_uv(tex_coord_grad);

    const f32x3x3 camera_relative_world_positions = {
        tri.world_positions[0].xyz - params.camera.position.xyz,
        tri.world_positions[1].xyz - params.camera.position.xyz,
        tri.world_positions[2].xyz - params.camera.position.xyz,
    };

    const f32x3 world_normal = tri.interpolate_normal(deriv.lambda);
    const TangentFrame frame = build_tangent_frame(
        world_normal,
        mul(deriv.ddx, camera_relative_world_positions),
        mul(deriv.ddy, camera_relative_world_positions),
        tex_coord_grad.ddx,
        tex_coord_grad.ddy
    );

    let surface = shade_surface(material, frame, tex_coord_grad);

    output.albedo_color = f32x4(surface.albedo.rgb, surface.occlusion);
    output.normal_color.xy = com::vec3_to_oct(surface.normal);
    output.normal_color.zw = com::vec3_to_oct(world_normal);
    output.emission_color = surface.emissive;
    output.metallic_roughness_color = f32x2(surface.metallic, surface.roughness);
    output.velocity = compute_velocity(tri, deriv.lambda, params.transforms_previous[tri.transform_index]);

    return output;
}
