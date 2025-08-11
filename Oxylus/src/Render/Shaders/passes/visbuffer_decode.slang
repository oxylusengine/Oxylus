module visbuffer_decode;

import common;
import gpu;
import scene;
import visbuffer;

#include <fullscreen.slang>

struct ShaderParameters {
    Image2D<u32> visbuffer;
    ConstantBuffer<Camera> camera;
    StructuredBuffer<MeshletInstance> meshlet_instances;
    StructuredBuffer<Mesh> meshes;
    StructuredBuffer<Transform> transforms;
    StructuredBuffer<Material> materials;
};

ParameterBlock<ShaderParameters> params;

struct FragmentOutput {
    f32x4 albedo_color : SV_Target0;
    f32x4 normal_color : SV_Target1;
    f32x3 emission_color : SV_Target2;
    f32x4 metallic_roughness_occlusion_color : SV_Target3;
};

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

    const let clip_pos_0 = mul(params.camera.projection_view, f32x4(world_positions[0].xyz, 1.0));
    const let clip_pos_1 = mul(params.camera.projection_view, f32x4(world_positions[1].xyz, 1.0));
    const let clip_pos_2 = mul(params.camera.projection_view, f32x4(world_positions[2].xyz, 1.0));

    // Partial Derivatives
    const f32x3 inv_w = 1.0 / f32x3(clip_pos_0.w, clip_pos_1.w, clip_pos_2.w);
    const f32x2 ndc_0 = clip_pos_0.xy * inv_w[0];
    const f32x2 ndc_1 = clip_pos_1.xy * inv_w[1];
    const f32x2 ndc_2 = clip_pos_2.xy * inv_w[2];
    // Inverse area of a triangle.
    // https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf
    // Appendix A:
    // D =
    const f32 inv_det = 1.0 / determinant(f32x2x2(ndc_2 - ndc_1, ndc_0 - ndc_1));
    // Lambda 1 =
    result.ddx = f32x3(ndc_1.y - ndc_2.y, ndc_2.y - ndc_0.y, ndc_0.y - ndc_1.y) * inv_det * inv_w;
    // Lambda 2 =
    result.ddy = f32x3(ndc_2.x - ndc_1.x, ndc_0.x - ndc_2.x, ndc_1.x - ndc_0.x) * inv_det * inv_w;
    f32 ddx_sum = dot(result.ddx, 1.0);
    f32 ddy_sum = dot(result.ddy, 1.0);

    const f32x2 delta_v = uv - ndc_0;
    const f32 interp_inv_w = inv_w.x + delta_v.x * ddx_sum + delta_v.y * ddy_sum;
    const f32 interp_w = 1.0 / interp_inv_w;
    result.lambda = f32x3(
        interp_w * (inv_w[0] + delta_v.x * result.ddx.x + delta_v.y * result.ddy.x),
        interp_w * (delta_v.x * result.ddx.y + delta_v.y * result.ddy.y),
        interp_w * (delta_v.x * result.ddx.z + delta_v.y * result.ddy.z)
    );
    const f32x2 two_over_resolution = 2.0 / resolution;
    result.ddx *= two_over_resolution.x;
    result.ddy *= -two_over_resolution.y;
    ddx_sum *= two_over_resolution.x;
    ddy_sum *= -two_over_resolution.y;

    const f32 interp_ddx_w = 1.0 / (interp_inv_w + ddx_sum);
    const f32 interp_ddy_w = 1.0 / (interp_inv_w + ddy_sum);
    result.ddx = interp_ddx_w * (result.lambda * interp_inv_w + result.ddx) - result.lambda;
    result.ddy = interp_ddy_w * (result.lambda * interp_inv_w + result.ddy) - result.lambda;

    return result;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> FragmentOutput {
    const u32 texel = params.visbuffer.load(u32x2(input.position.xy));
    if (texel == ~0u) {
        discard;
    }

    FragmentOutput output = {};

    const let vis = VisBufferData(texel);
    const MeshletInstance meshlet_instance = params.meshlet_instances[vis.meshlet_instance_index];
    const Mesh mesh = params.meshes[meshlet_instance.mesh_index];
    const Transform transform = params.transforms[meshlet_instance.transform_index];
    const Material material = params.materials[meshlet_instance.material_index];

    const Meshlet meshlet = mesh.meshlets[meshlet_instance.meshlet_index];
    const Triangle indices = meshlet.indices(mesh, vis.triangle_index);
    const u32x3 vertices = meshlet.vertices(mesh, indices);
    const f32x3x3 positions = meshlet.positions(mesh, vertices);
    const f32x3x3 normals = meshlet.normals(mesh, vertices);
    const f32x2x3 tex_coords = meshlet.tex_coords(mesh, vertices);
    const f32x4x3 world_positions = transform.to_world_positions(positions);
    const f32x3 NDC = f32x3(input.tex_coord * 2.0 - 1.0, 1.0);
    const PartialDeriv deriv = compute_partial_derivatives(world_positions, NDC.xy, params.camera.resolution);
    const UVGradient tex_coord_grad = deriv.gradient_of(tex_coords);

    // ALBEDO ───────────────────────────────────────────────────────────
    output.albedo_color = material.sample_albedo_color(tex_coord_grad);

    // NORMALS ──────────────────────────────────────────────────────────
    const f32x3x3 camera_relative_world_positions = { world_positions[0].xyz - params.camera.position.xyz,
                                                      world_positions[1].xyz - params.camera.position.xyz,
                                                      world_positions[2].xyz - params.camera.position.xyz };
    const f32x3 pos_ddx = mul(deriv.ddx, camera_relative_world_positions);
    const f32x3 pos_ddy = mul(deriv.ddy, camera_relative_world_positions);

    const f32x3x3 world_normals = transform.to_world_normals(normals);
    const f32x3 world_normal = mul(deriv.lambda, world_normals);

    const f32x3 pos_ddx_s = pos_ddx - dot(pos_ddx, world_normal) * world_normal;
    const f32x3 pos_ddy_s = pos_ddy - dot(pos_ddy, world_normal) * world_normal;
    const f32x2 uv_ddx = tex_coord_grad.ddx;
    const f32x2 uv_ddy = tex_coord_grad.ddy;

    const i32 jacobian_sign = sign(uv_ddx.x * uv_ddy.y - uv_ddx.y * uv_ddy.x);
    f32x3 tangent = jacobian_sign * (uv_ddy.y * pos_ddx_s - uv_ddx.y * pos_ddy_s);
    if (jacobian_sign != 0.0) {
        tangent = normalize(tangent);
    }
    const f32 w = jacobian_sign * sign(dot(pos_ddy, cross(world_normal, pos_ddx)));
    f32x3 bitangent = -w * cross(world_normal, tangent);
    f32x3 normal = world_normal;

#if 1
    const f32 inv_len_N = 1.0 / length(normal);
    tangent *= inv_len_N;
    bitangent *= inv_len_N;
    normal *= inv_len_N;
#endif

    if (material.flags & MaterialFlag::HasNormalImage) {
        f32x3 sampled_normal = material.sample_normal_color(tex_coord_grad);
        // NOTE: This is here to convert tangent spaces to our correct spaces.
        // for example, normal map's R component is +Y for our world. @grok is this true?
        sampled_normal = f32x3(sampled_normal.y, sampled_normal.x, sampled_normal.z);

        if (material.flags & MaterialFlag::NormalTwoComponent) {
            sampled_normal = f32x3(sampled_normal.yx * 2.0 - 1.0, 0.0);
            sampled_normal.z = sqrt(1.0 - sampled_normal.x * sampled_normal.x - sampled_normal.y * sampled_normal.y);
        } else {
            sampled_normal = sampled_normal * 2.0 - 1.0;
        }

        if (material.flags & MaterialFlag::NormalFlipY) {
            sampled_normal.y = -sampled_normal.y;
        }

        normal = sampled_normal.x * tangent
               + sampled_normal.y * bitangent
               + sampled_normal.z * normal;
        normal = normalize(normal);
    }

    output.normal_color.xy = com::vec3_to_oct(normal);
    output.normal_color.zw = com::vec3_to_oct(world_normal);

    // EMISSION ─────────────────────────────────────────────────────────
    output.emission_color = material.sample_emissive_color(tex_coord_grad);

    // METALLIC ROUGHNESS ───────────────────────────────────────────────
    const f32x2 metallic_roughness_color = material.sample_metallic_roughness(tex_coord_grad);
    output.metallic_roughness_occlusion_color.xy = metallic_roughness_color;

    // AMBIENT OCCLUSION ────────────────────────────────────────────────
    const f32 occlusion_color = material.sample_occlusion_color(tex_coord_grad);
    output.metallic_roughness_occlusion_color.z = occlusion_color;

    return output;
}
