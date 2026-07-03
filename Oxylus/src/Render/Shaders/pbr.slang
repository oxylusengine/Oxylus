import common;

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

func GGX_directional_albedo(f32 NoV, f32 alpha) -> f32x2 {
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
