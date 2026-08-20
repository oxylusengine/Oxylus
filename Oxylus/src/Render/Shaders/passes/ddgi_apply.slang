import common;
import gpu;
import pbr;
import scene;
import ddgi;

#include <fullscreen.slang>

[[vk::binding(0, 0)]] Sampler linear_clamp_sampler;
[[vk::binding(1, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(2, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(3, 0)]] Texture2D<f32> depth_image;
[[vk::binding(4, 0)]] Texture2D<f32x4> albedo_image;
[[vk::binding(5, 0)]] Texture2D<f32x4> normal_image;
[[vk::binding(6, 0)]] Texture2D<f32x3> metallic_roughness_occlusion_image;
[[vk::binding(7, 0)]] Texture2D<f32> gtao_image;
[[vk::binding(8, 0)]] Texture2D<f32x4> irradiance_atlas;
[[vk::binding(9, 0)]] Texture2D<f32x2> distance_atlas;
[[vk::binding(10, 0)]] StructuredBuffer<ProbeState> probe_states;

[[shader("fragment")]]
func fs_main(
    VertexOutput input,
    uniform u32 volume_count,
    uniform f32 normal_bias,
    uniform f32 view_bias,
    uniform f32 intensity,
    uniform f32x3 ambient_color
) -> f32x4 {
    let pixel_pos = u32x3(u32x2(input.position.xy), 0);
    let depth = depth_image.Load(pixel_pos);
    if (depth == 0.0) {
        return f32x4(0.0);
    }

    let albedo_color = albedo_image.SampleLevel(linear_clamp_sampler, input.tex_coord, 0).rgb;

    let mapped_smooth_normal = normal_image.Load(pixel_pos);
    let N = normalize(com::oct_to_vec3(mapped_smooth_normal.rg));

    let metallic_roughness_occlusion = metallic_roughness_occlusion_image.Load(pixel_pos);
    let metallic = clamp(metallic_roughness_occlusion.r, 0.0, 1.0);
    let roughness = clamp(metallic_roughness_occlusion.g, 0.045, 1.0);
    let occlusion = metallic_roughness_occlusion.b * gtao_image.Load(pixel_pos).r;

    let world_position = camera.unproject_uv(input.tex_coord, depth);
    let V = normalize(camera.position.xyz - world_position);

    let R = reflect(-V, N);
    let reflection_direction = normalize(lerp(N, R, 1.0 - roughness));

    let sample = sample_ddgi_irradiance(
        probe_volumes,
        probe_states,
        volume_count,
        irradiance_atlas,
        distance_atlas,
        linear_clamp_sampler,
        world_position,
        N,
        V,
        reflection_direction,
        true,
        normal_bias,
        view_bias
    );

    let diffuse = lerp(ambient_color, sample.irradiance * intensity, sample.coverage);
    let specular = lerp(ambient_color, sample.specular * intensity, sample.coverage);

    let NoV = abs(dot(N, V)) + 1e-5;
    let F0 = lerp(f32x3(0.04), albedo_color, metallic);
    let AB = GGX_directional_albedo(NoV, perceptual_roughness_to_alpha(roughness));
    let kS = F0 * AB.x + AB.y;
    let kD = (1.0 - metallic) * (1.0 - kS);
    let spec_occlusion = specular_occlusion_horizon(NoV, occlusion, roughness);

    let indirect_diffuse = kD * diffuse * albedo_color * occlusion;
    let indirect_specular = kS * specular * spec_occlusion;

    return f32x4(indirect_diffuse + indirect_specular, 1.0);
}
