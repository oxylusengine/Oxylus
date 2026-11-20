import gpu;
import common;
import pbr;
import sky;
import scene;
import ddgi;
import hit;
import lighting;
import fog;

#include <defines.slang>

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::constant_id(1)]] const u32 has_directional_shadows = 0;
[[vk::constant_id(3)]] const u32 has_refractor_split = 0;

[[vk::binding(0, 0)]] Sampler linear_clamp_sampler;
[[vk::binding(1, 0)]] Sampler nearest_clamp_sampler;
[[vk::binding(2, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(23, 0)]] Texture3D<f32x4> froxels;
[[vk::binding(3, 0)]] StructuredBuffer<Material> materials;
[[vk::binding(4, 0)]] Texture2DArray<u32> vsm_page_table;
[[vk::binding(5, 0)]] Texture2DArray<u32> vsm_pointspot_page_tables;
[[vk::binding(6, 0)]] Texture2D<f32> vsm_physical_pages;
[[vk::binding(7, 0)]] Texture2D<f32x4> sky_transmittance_lut;
[[vk::binding(8, 0)]] Texture3D<f32x4> sky_aerial_perspective_lut;
[[vk::binding(9, 0)]] TextureCube<f32x4> sky_cubemap;
[[vk::binding(10, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(11, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(12, 0)]] Texture2D<f32x4> irradiance_atlas;
[[vk::binding(13, 0)]] Texture2D<f32x2> distance_atlas;
[[vk::binding(20, 0)]] Texture2D<f32> refractor_depth;

struct ForwardPush {
    // the sky instead when the scene has no atmosphere, the shaders never read both
    Atmosphere *atmosphere;
    PBRLighting *lighting;
    Light *lights;
    LightGridCell *light_grid;
    VSMPointSpotView *vsm_pointspot_views;
    VirtualClipmap *vsm_clipmaps;
    u32 probe_volume_count;
    f32 ddgi_normal_bias;
    f32 ddgi_view_bias;
    f32 ddgi_intensity;
    f32x3 ddgi_ambient_color;
};

struct ForwardVertex {
    f32x4 position : SV_Position;
    f32x3 world_position;
    f32x3 world_normal;
    f32x2 tex_coord;
    nointerpolation u32 material_index;
};

struct ForwardOutput {
    f32x4 accumulation : SV_Target0;
    f32 revealage : SV_Target1;
    f32x4 accumulation_front : SV_Target2;
    f32 revealage_front : SV_Target3;
};

struct SurfaceLight {
    f32x3 diffuse;
    f32x3 specular;
    f32x3 aerial_perspective;
    // the share of `specular` that came from the environment, and its weight, so a traced reflection can replace it
    f32x3 environment_specular;
    f32x3 environment_weight;
    // what a white lambertian surface would reflect, lights a participating medium
    f32x3 scatter_radiance;
};

func light_surface(
    in SurfaceMaterial surface,
    f32 ior,
    f32x3 smooth_normal,
    f32x3 world_position,
    f32x2 frag_coord,
    f32 depth,
    in ForwardPush push
) -> SurfaceLight {
    let albedo_color = surface.albedo.rgb;
    let metallic = clamp(surface.metallic, 0.0, 1.0);
    let roughness = clamp(surface.roughness, 0.045, 1.0);
    let N = surface.normal;
    let relative_position = world_position - camera.position.xyz;

    let uv = frag_coord / camera.resolution;
    let lighting_resources = LightingResources(
        vsm_page_table,
        vsm_pointspot_page_tables,
        vsm_physical_pages,
        nearest_clamp_sampler,
        camera,
        push.lighting,
        push.lights,
        push.light_grid,
        push.vsm_pointspot_views,
        push.vsm_clipmaps
    );

    let L = push.lighting.sun_direction;
    let Li = push.lighting.sun_intensity;
    let V = normalize(camera.position.xyz - world_position);
    let NoL = max(dot(N, L), 0.0);
    let has_directional_light = (scene_flags & SceneFlags::HasDirectionalLight) != 0;
    let is_atmosphere = (scene_flags & SceneFlags::HasAtmosphere) != 0;

    var visibility = 0.0;
    if (NoL > 0.0) {
        visibility = lighting_directional_visibility(
            lighting_resources,
            scene_flags,
            has_directional_shadows,
            uv,
            frag_coord,
            depth,
            smooth_normal,
            world_position,
            has_directional_light
        );
    }

    let sun_cos_theta = dot(L, f32x3(0.0, 1.0, 0.0));
    let horizon_darkening = select(is_atmosphere, smoothstep(-0.1, 0.3, sun_cos_theta), 1.0);

    var direct_illuminance = select(has_directional_light, f32x3(Li), f32x3(0.0));
    var aerial_perspective_contribution = f32x3(0.0);
    if (is_atmosphere) {
        let world_eye_pos = push.atmosphere.get_eye_pos(world_position);
        let sun_transmittance = get_direct_sun_lighting(
            push.atmosphere, linear_clamp_sampler, sky_transmittance_lut, world_eye_pos.y, sun_cos_theta);
        direct_illuminance = sun_transmittance * Li * horizon_darkening;

        let aerial_perspective = sample_aerial_perspective(
            push.atmosphere,
            sky_aerial_perspective_lut,
            linear_clamp_sampler,
            uv,
            relative_position * CAMERA_SCALE_UNIT);
        aerial_perspective_contribution = aerial_perspective.rgb * aerial_perspective.a;
    }

    let R = reflect(-V, N);
    let NoV = abs(dot(N, V)) + 1e-5;
    let alpha = perceptual_roughness_to_alpha(roughness);
    let AB = GGX_directional_albedo(NoV, alpha);
    let brdf_params = brdf_setup(alpha, AB);

    let dielectric_f0_scalar = dielectric_f0(ior);
    let F0 = lerp(f32x3(dielectric_f0_scalar), albedo_color, metallic);
    let kS = F0 * AB.x + AB.y;
    let kD = (1.0 - metallic) * (1.0 - kS);
    let spec_occlusion = specular_occlusion_horizon(NoV, surface.occlusion, roughness);

    var diffuse_environment = f32x3(0.0);
    var specular_environment = f32x3(0.0);
    if ((scene_flags & SceneFlags::HasDDGI) != 0) {
        ProbeSampleParams params;
        params.world_position = world_position;
        params.normal = N;
        params.geometric_normal = smooth_normal;
        params.view_direction = V;
        params.view_distance = length(camera.position.xyz - world_position);
        params.specular_direction = normalize(lerp(N, R, 1.0 - roughness));
        params.want_specular = true;
        params.normal_bias = push.ddgi_normal_bias;
        params.view_bias = push.ddgi_view_bias;

        let probe_sample = sample_ddgi_irradiance(
            probe_volumes,
            probe_states,
            push.probe_volume_count,
            irradiance_atlas,
            distance_atlas,
            linear_clamp_sampler,
            params
        );
        diffuse_environment = lerp(push.ddgi_ambient_color, probe_sample.irradiance * push.ddgi_intensity, probe_sample.coverage);
        specular_environment = lerp(push.ddgi_ambient_color, probe_sample.specular * push.ddgi_intensity, probe_sample.coverage);
    } else if (is_atmosphere) {
        let diffuse_sample = sky_cubemap.SampleLevel(linear_clamp_sampler, N, 0.0);
        diffuse_environment = diffuse_sample.rgb * diffuse_sample.a * horizon_darkening;
        specular_environment = sky_cubemap_specular(sky_cubemap, linear_clamp_sampler, N, R, roughness, horizon_darkening);
    } else {
        let has_sky = (scene_flags & SceneFlags::HasSky) != 0;
        let sky = (Sky *)push.atmosphere;
        diffuse_environment = select(has_sky, sky.ambient_color, push.lighting.base_ambient_color);
        specular_environment = diffuse_environment;
    }

    let indirect_diffuse = kD * diffuse_environment * albedo_color * surface.occlusion;
    let indirect_specular = kS * specular_environment * spec_occlusion;

    let horizon_fade = 1.3;
    var horizon = saturate(1.0 + horizon_fade * dot(R, smooth_normal));
    horizon *= horizon;

    SurfaceLight light;
    light.diffuse = indirect_diffuse;
    light.specular = indirect_specular;
    light.aerial_perspective = aerial_perspective_contribution;
    light.environment_specular = specular_environment;
    light.environment_weight = kS * spec_occlusion;
    light.scatter_radiance = diffuse_environment + direct_illuminance * visibility / PI;
    if (NoL > 0.0) {
        let result = brdf_evaluate(brdf_params, V, N, L, albedo_color, metallic, dielectric_f0_scalar);
        let direct_scale = direct_illuminance * NoL * visibility;
        light.diffuse += result.diffuse * direct_scale;
        light.specular += result.specular * horizon * direct_scale;
    }

    lighting_point_spot(
        lighting_resources,
        uv,
        depth,
        world_position,
        N,
        V,
        smooth_normal,
        albedo_color,
        metallic,
        dielectric_f0_scalar,
        brdf_params,
        light.diffuse,
        light.specular);

    return light;
}

func shade_transparent(in ForwardVertex input, bool is_front_face, in ForwardPush push) -> ForwardOutput {
    let material = materials[NonUniformResourceIndex(input.material_index)];

    UVGradient uv_grad;
    uv_grad.uv = input.tex_coord;
    uv_grad.ddx = ddx(input.tex_coord);
    uv_grad.ddy = ddy(input.tex_coord);
    material.transform_uv(uv_grad);

    let relative_position = input.world_position - camera.position.xyz;
    let position_dx = ddx(relative_position);
    let position_dy = ddy(relative_position);

    let is_flipped = !is_front_face && material.is_double_sided();
    let smooth_normal = normalize(input.world_normal) * select(is_flipped, -1.0, 1.0);
    let frame = build_tangent_frame(smooth_normal, position_dx, position_dy, uv_grad.ddx, uv_grad.ddy);

    // biased after the tangent frame, which needs the true uv derivatives
    uv_grad.apply_mip_bias(camera.texture_mip_bias);
    var surface = shade_surface(material, frame, uv_grad);

    // every derivative is taken before the discard, quad neighbours must still be running
    let normal_dx = ddx(surface.normal);
    let normal_dy = ddy(surface.normal);
    surface.roughness = filter_specular_roughness(surface.roughness, normal_dx, normal_dy);

    let coverage = saturate(surface.albedo.a);
    if (coverage < 1.0 / 255.0) {
        discard;
    }

    let light = light_surface(
        surface, material.get_ior(), smooth_normal, input.world_position, input.position.xy, input.position.z, push);
    var color = light.diffuse + light.specular + surface.emissive + light.aerial_perspective;
    // the opaque scene behind already carries its own fog, so the surface takes only what lies in front of it
    let screen_uv = input.position.xy / camera.resolution;
    color = evaluate_fog(camera, input.world_position, screen_uv, froxels, linear_clamp_sampler).apply(color);

    // the resolve divides by the summed weight so only its ratios matter, scaled to peak at 1 so fp16 keeps the hdr range
    let view_distance = length(relative_position);
    let weight = clamp(
        10.0 / (1e-5 + pow(view_distance / 5.0, 2.0) + pow(view_distance / 200.0, 6.0)),
        1e-2,
        3e3
    ) / 3e3;
    color = clamp(com::sanitize(color), f32x3(0.0), f32x3(8000.0));

    let weighted = f32x4(color * coverage, coverage) * weight;
    let is_split = has_refractor_split != 0;
    let is_behind_refractor = is_split && input.position.z < refractor_depth.Load(i32x3(i32x2(input.position.xy), 0));
    // a zero leaves the other pair's blend untouched
    let is_rear = !is_split || is_behind_refractor;

    ForwardOutput output;
    output.accumulation = select(is_rear, weighted, f32x4(0.0));
    output.revealage = select(is_rear, coverage, 0.0);
    output.accumulation_front = select(is_rear, f32x4(0.0), weighted);
    output.revealage_front = select(is_rear, 0.0, coverage);

    return output;
}
