import common;
import scene;

public static constexpr f32 MIN_ALPHA = 0.0025;

public f32 perceptual_roughness_to_alpha(f32 perceptual_roughness) {
    let alpha = perceptual_roughness * perceptual_roughness;
    return max(alpha, MIN_ALPHA);
}

public f32 D_GGX(f32 NoH, f32 alpha2) {
    let f = (NoH * alpha2 - NoH) * NoH + 1.0;
    return alpha2 / (PI * f * f + 1e-7);
}

public f32 V_SmithGGXCorrelated(f32 NoV, f32 NoL, f32 alpha2) {
    let GGXV = NoL * sqrt(NoV * NoV * (1.0 - alpha2) + alpha2);
    let GGXL = NoV * sqrt(NoL * NoL * (1.0 - alpha2) + alpha2);
    return saturate(0.5 / (GGXV + GGXL + 1e-7));
}

public f32x3 F_Schlick(f32 LoH, f32x3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - LoH), 5.0);
}

public f32x3 F_SchlickRoughness(f32 NoV, f32x3 F0, f32 perceptual_roughness) {
    let F90 = max(f32x3(1.0 - perceptual_roughness), F0);
    return F0 + (F90 - F0) * pow(saturate(1.0 - NoV), 5.0);
}

public constexpr f32 Fd_Lambert() {
    return 1.0 / PI;
}

public func GGX_directional_albedo(f32 NoV, f32 alpha) -> f32x2 {
    let x = NoV;
    let y = alpha;
    let x2 = x * x;
    let y2 = y * y;
    const let r = f32x4(0.1003, 0.9345, 1.0, 1.0) + f32x4(-0.6303, -2.323, -1.765, 0.2281) * x + f32x4(9.748, 2.229, 8.263, 15.94) * y
        + f32x4(-2.038, -3.748, 11.53, -55.83) * x * y + f32x4(29.34, 1.424, 28.96, 13.08) * x2 + f32x4(-8.245, -0.7684, -7.507, 41.26) * y2
        + f32x4(-26.44, 1.436, -36.11, 54.9) * x2 * y + f32x4(19.99, 0.2913, 15.86, 300.2) * x * y2 + f32x4(-5.448, 0.6286, 33.37, -285.1) * x2 * y2;
    return clamp(r.xy / r.zw, 0.0, 1.0);
}

public f32x3 GGX_energy_compensation(f32 NoV, f32 alpha, f32x3 Fss) {
    let AB = GGX_directional_albedo(NoV, alpha);
    let Ess = saturate(AB.x + AB.y);
    return 1.0 + Fss * (1.0 - Ess) / max(Ess, 1e-4);
}

public f32 specular_occlusion_horizon(f32 NoV, f32 ao, f32 perceptual_roughness) {
    return saturate(pow(NoV + ao, exp2(-16.0 * perceptual_roughness - 1.0)) - 1.0 + ao);
}

public struct BRDFResult {
    public f32x3 diffuse;
    public f32x3 specular;
};

public func BRDF(f32x3 V, f32x3 N, f32x3 L, f32x3 albedo, f32 perceptual_roughness, f32 metallic) -> BRDFResult {
    let VL = V + L;
    let H = dot(VL, VL) > 1e-8 ? normalize(VL) : N;
    let NoV = abs(dot(N, V)) + 1e-5;
    let NoL = saturate(dot(N, L));
    let NoH = saturate(dot(N, H));
    let LoH = saturate(dot(L, H));

    let dielectric_F0 = f32x3(0.04);
    let F0 = lerp(dielectric_F0, albedo, metallic);

    let alpha = perceptual_roughness_to_alpha(perceptual_roughness);
    let alpha2 = alpha * alpha;

    let D = D_GGX(NoH, alpha2);
    let Vis = V_SmithGGXCorrelated(NoV, NoL, alpha2);
    let F = F_Schlick(LoH, F0);
    let energy_compensation = GGX_energy_compensation(NoV, alpha, F0);

    BRDFResult result;
    result.specular = D * Vis * F * energy_compensation;

    let kd = (1.0 - metallic) * (1.0 - F);
    result.diffuse = kd * albedo * Fd_Lambert();

    return result;
}

public func lights_attenuate_point(f32 to_light_dist, f32 cutoff) -> f32 {
    if (cutoff <= 0.0) {
        return 1.0 / (to_light_dist * to_light_dist + 0.1f);
    }

    f32 win = (to_light_dist / cutoff);
    win = win * win * win * win;
    win = max(0.0, 1.0 - win);
    win = win * win;
    f32 attenuation = win / (to_light_dist * to_light_dist + 0.1);
    return attenuation;
}

public f32x3 point_light_contribution(
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 albedo_color,
    f32 roughness,
    f32 metallic,
    f32x3 light_position,
    Light light
) {
    f32x3 light_vector = light_position - world_position;
    f32 light_distance = length(light_vector);
    f32x3 L = light_vector / light_distance;

    f32 attenuation = lights_attenuate_point(light_distance, light.range);
    if (attenuation <= 0.0 || light.intensity <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    f32 NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    let result = BRDF(V, N, L, albedo_color, roughness, metallic);
    f32x3 radiance = light.color * attenuation * light.intensity;

    return (result.diffuse + result.specular) * radiance * NdotL;
}

public f32 calculate_spot_light_attenuation(f32x3 light_direction, f32x3 spot_direction, f32 inner_cone_angle, f32 outer_cone_angle) {
    f32 cos_angle = dot(-light_direction, normalize(spot_direction));
    f32 cos_inner = cos(inner_cone_angle);
    f32 cos_outer = cos(outer_cone_angle);
    f32 spot_attenuation = smoothstep(cos_outer, cos_inner, cos_angle);
    return spot_attenuation;
}

public f32x3 calculate_spot_light_contribution(
    f32x3 world_position,
    f32x3 N,
    f32x3 V,
    f32x3 albedo_color,
    f32 roughness,
    f32 metallic,
    f32x3 light_position,
    f32x3 light_direction,
    Light light
) {
    f32x3 light_vector = light_position - world_position;
    f32 light_distance = length(light_vector);
    f32x3 L = light_vector / light_distance;

    f32 distance_attenuation = lights_attenuate_point(light_distance, light.range);
    f32 spot_attenuation = calculate_spot_light_attenuation(L, light_direction, light.inner_cone_angle, light.outer_cone_angle);
    f32 total_attenuation = distance_attenuation * spot_attenuation;

    if (total_attenuation <= 0.0 || light.intensity <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    f32 NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) {
        return f32x3(0.0, 0.0, 0.0);
    }

    let result = BRDF(V, N, L, albedo_color, roughness, metallic);
    f32x3 radiance = light.color * total_attenuation * light.intensity;

    return (result.diffuse + result.specular) * radiance * NdotL;
}
