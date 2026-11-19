import common;
import gpu;
import scene;
import visbuffer;
import hit;
import pbr;
import decal;

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

struct DecodePush {
    Decal *decals;
    u32 decal_count;
};
[[vk::push_constant]] DecodePush push;

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

    // evaluate attributes at the same subpixel position used by visibility rasterization
    let sample_uv = input.tex_coord - params.camera.temporalaa_jitter / params.camera.resolution;
    let NDC = f32x3(sample_uv * 2.0 - 1.0, 1.0);
    let decoded = decode_triangle_surface(tri, material, NDC.xy, params.camera);
    let deriv = decoded.triangle.deriv;
    let world_normal = decoded.triangle.world_normal;
    let surface = decoded.surface;

    DecalTarget target;
    target.albedo = surface.albedo.rgb;
    target.normal = surface.normal;
    target.emissive = surface.emissive;
    target.metallic = surface.metallic;
    target.roughness = surface.roughness;
    apply_decals(
        target,
        push.decals,
        push.decal_count,
        params.materials,
        tri.interpolate_position(deriv.lambda),
        decoded.position_dx,
        decoded.position_dy,
        world_normal,
        params.mesh_instances[tri.mesh_instance_index].layer_mask,
        params.camera.texture_mip_bias
    );

    output.albedo_color = f32x4(target.albedo, surface.occlusion);
    output.normal_color.xy = com::vec3_to_oct(target.normal);
    output.normal_color.zw = com::vec3_to_oct(world_normal);
    output.emission_color = target.emissive;
    output.metallic_roughness_color = f32x2(target.metallic, target.roughness);
    output.velocity = compute_velocity(tri, deriv.lambda, params.transforms_previous[tri.transform_index]);

    return output;
}
