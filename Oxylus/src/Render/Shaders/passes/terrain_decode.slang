import common;
import gpu;
import scene;
import visbuffer;

#include <fullscreen.slang>

struct ShaderParameters {
    ConstantBuffer<Camera> camera;
    StructuredBuffer<Material> materials;
    ConstantBuffer<TerrainData> terrain;

    Image2D<u32> visbuffer;
    Image2D<f32> depth;
    Image2D<f32x2> terrain_normalmap;
    Image2D<f32x4> terrain_splatmap;
    SamplerState terrain_sampler;
    StructuredBuffer<TerrainBrushHit> brush_hit;
};
ParameterBlock<ShaderParameters> params;

struct FragmentOutput {
    f32x4 albedo_color : SV_Target0;
    f32x4 normal_color : SV_Target1;
    f32x3 emission_color : SV_Target2;
    f32x2 metallic_roughness_color : SV_Target3;
};

struct TerrainSurface {
    f32x4 albedo;
    f32x3 emissive;
    f32x2 metallic_roughness;
    f32 occlusion;
    f32x3 tangent_normal;
};

// blends the four splat layers at one projection
func sample_terrain_layers(f32x2 uv, f32x2 ddx_uv, f32x2 ddy_uv, f32x4 weights) -> TerrainSurface {
    UVGradient grad;
    grad.uv = uv;
    grad.ddx = ddx_uv;
    grad.ddy = ddy_uv;

    TerrainSurface result = {};
    for (var i = 0; i < 4; i++) {
        let weight = weights[i];
        if (weight <= 0.0) {
            continue;
        }

        let material_index = params.terrain.layer_material_indices[i];
        if (material_index == TERRAIN_INVALID_LAYER_MATERIAL) {
            result.albedo += f32x4(1.0) * weight;
            result.metallic_roughness += f32x2(0.0, 1.0) * weight;
            result.occlusion += weight;
            result.tangent_normal += f32x3(0.0, 0.0, 1.0) * weight;
            continue;
        }

        let material = params.materials[NonUniformResourceIndex(material_index)];
        result.albedo += material.sample_albedo_color(grad) * weight;
        result.emissive += material.sample_emissive_color(grad) * weight;
        result.metallic_roughness += material.sample_metallic_roughness(grad) * weight;
        result.occlusion += material.sample_occlusion_color(grad) * weight;

        result.tangent_normal += material.sample_tangent_normal(grad) * weight;
    }

    return result;
}

func decode_terrain(f32x2 uv) -> FragmentOutput {
    let depth = params.depth.Load(u32x3(u32x2(uv * params.camera.resolution), 0));
    let world_position = params.camera.unproject_uv(uv, depth);
    let camera_position = params.camera.position.xyz;

    let terrain_uv = params.terrain.world_to_uv(world_position.xz);
    let normal_xz = params.terrain_normalmap.SampleLevel(params.terrain_sampler, terrain_uv, 0.0);
    let geometric_normal = normalize(f32x3(normal_xz.x, sqrt(saturate(1.0 - dot(normal_xz, normal_xz))), normal_xz.y));

    let texel = 1.0 / params.camera.resolution;
    let plane_offset = dot(world_position - camera_position, geometric_normal);
    let ray_center = world_position - camera_position;

    f32x3 neighbour_offsets[2];
    [[unroll]]
    for (var i = 0; i < 2; i++) {
        let neighbour_uv = uv + (i == 0 ? f32x2(texel.x, 0.0) : f32x2(0.0, texel.y));
        let ray = params.camera.unproject_uv(neighbour_uv, 0.5) - camera_position;
        let denominator = dot(ray, geometric_normal);
        let safe_denominator = abs(denominator) < 1e-6 ? 1e-6 : denominator;
        let hit = ray * (plane_offset / safe_denominator);
        neighbour_offsets[i] = abs(denominator) < 1e-6 ? ray_center : hit;
    }
    let ddx_world = neighbour_offsets[0] - ray_center;
    let ddy_world = neighbour_offsets[1] - ray_center;

    var weights = params.terrain_splatmap.SampleLevel(params.terrain_sampler, terrain_uv, 0.0);
    let weight_sum = dot(weights, 1.0);
    weights = weight_sum > 1e-4 ? weights / weight_sum : f32x4(1.0, 0.0, 0.0, 0.0);

    let inv_tiling = 1.0 / params.terrain.layer_tiling;
    let slope = saturate(1.0 - geometric_normal.y);
    let triplanar_amount = smoothstep(params.terrain.triplanar_begin, 1.0, slope);

    var surface = sample_terrain_layers(
        world_position.xz * inv_tiling, ddx_world.xz * inv_tiling, ddy_world.xz * inv_tiling, weights
    );
    var tangent = f32x3(1.0, 0.0, 0.0);
    var bitangent = f32x3(0.0, 0.0, -1.0);

    if (triplanar_amount > 0.0) {
        let use_zy = abs(geometric_normal.x) > abs(geometric_normal.z);
        let side_uv = use_zy ? world_position.zy : world_position.xy;
        let side_ddx = use_zy ? ddx_world.zy : ddx_world.xy;
        let side_ddy = use_zy ? ddy_world.zy : ddy_world.xy;

        let side = sample_terrain_layers(
            side_uv * inv_tiling, side_ddx * inv_tiling, side_ddy * inv_tiling, weights
        );

        surface.albedo = lerp(surface.albedo, side.albedo, triplanar_amount);
        surface.emissive = lerp(surface.emissive, side.emissive, triplanar_amount);
        surface.metallic_roughness = lerp(surface.metallic_roughness, side.metallic_roughness, triplanar_amount);
        surface.occlusion = lerp(surface.occlusion, side.occlusion, triplanar_amount);
        surface.tangent_normal = lerp(surface.tangent_normal, side.tangent_normal, triplanar_amount);

        // Side projections are (z, y) and (x, y), so +u is +Z or +X and -v is -Y for both.
        let side_tangent = use_zy ? f32x3(0.0, 0.0, 1.0) : f32x3(1.0, 0.0, 0.0);
        tangent = lerp(tangent, side_tangent, triplanar_amount);
        bitangent = lerp(bitangent, f32x3(0.0, -1.0, 0.0), triplanar_amount);
    }

    if (abs(dot(normalize(tangent), geometric_normal)) > 0.999) {
        tangent = f32x3(0.0, 0.0, 1.0);
    }
    tangent = normalize(tangent - geometric_normal * dot(tangent, geometric_normal));

    bitangent -= geometric_normal * dot(bitangent, geometric_normal);
    bitangent -= tangent * dot(bitangent, tangent);
    let bitangent_length = length(bitangent);
    bitangent = bitangent_length > 1e-6 ? bitangent / bitangent_length : cross(geometric_normal, tangent);

    let tangent_normal_length = length(surface.tangent_normal);
    let tangent_normal = tangent_normal_length > 1e-6 ? surface.tangent_normal / tangent_normal_length
                                                      : f32x3(0.0, 0.0, 1.0);
    let shading_normal = normalize(
        tangent_normal.x * tangent + tangent_normal.y * bitangent + tangent_normal.z * geometric_normal
    );

    if (params.terrain.brush_radius > 0.0 && params.brush_hit[0].valid != 0u) {
        let ring_distance = distance(world_position.xz, params.brush_hit[0].world_position.xz);
        let ring_width = max(params.terrain.brush_radius * 0.02, 0.05);
        let ring = 1.0 - smoothstep(0.0, ring_width, abs(ring_distance - params.terrain.brush_radius));

        surface.albedo.rgb = lerp(surface.albedo.rgb, f32x3(1.0, 0.55, 0.1), ring);
    }

    FragmentOutput output = {};
    output.albedo_color = f32x4(surface.albedo.rgb, surface.occlusion);
    output.normal_color.xy = com::vec3_to_oct(shading_normal);
    output.normal_color.zw = com::vec3_to_oct(geometric_normal);
    output.emission_color = surface.emissive;
    output.metallic_roughness_color = surface.metallic_roughness;

    return output;
}

[[shader("fragment")]]
func fs_main(VertexOutput input) -> FragmentOutput {
    let texel = params.visbuffer.Load(u32x3(u32x2(input.position.xy), 0));
    if (!is_terrain(texel)) {
        discard;
    }

    return decode_terrain(input.tex_coord);
}
