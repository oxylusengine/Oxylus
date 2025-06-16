module pbr;

import common;

public f32 D_GGX(f32 NoH, f32 roughness2) {
    f32 f = ((roughness2 - 1.0) * NoH * NoH + 1.0);
    return roughness2 / (PI * f * f);
}

public f32 Smith_G2_Height_Correlated_GGX_Lagarde(f32 NoV, f32 NoL, f32 roughness2) {
    f32 GGXL = NoV * sqrt((-NoL * roughness2 + NoL) * NoL + roughness2);
    f32 GGXV = NoL * sqrt((-NoV * roughness2 + NoV) * NoV + roughness2);
    return saturate(0.5f / (GGXV + GGXL));
}

public f32x3 F_Schlick(f32 LoH, f32x3 specular_albedo) {
    return specular_albedo + (1.0 - specular_albedo) * pow(1.0 - LoH, 5.0);
}

public constexpr f32 Fd_Lambert() {
    return 1.0 / PI;
}

public func GGX_Albedo_Analytic(f32 NdotV, f32 alpha, f32 F0, f32 F90) -> f32x3 {
    f32 x = NdotV;
    f32 y = alpha;
    f32 x2 = x * x;
    f32 y2 = y * y;
    const let r = f32x4(0.1003, 0.9345, 1.0, 1.0) + f32x4(-0.6303, -2.323, -1.765, 0.2281) * x + f32x4(9.748, 2.229, 8.263, 15.94) * y
        + f32x4(-2.038, -3.748, 11.53, -55.83) * x * y + f32x4(29.34, 1.424, 28.96, 13.08) * x2 + f32x4(-8.245, -0.7684, -7.507, 41.26) * y2
        + f32x4(-26.44, 1.436, -36.11, 54.9) * x2 * y + f32x4(19.99, 0.2913, 15.86, 300.2) * x * y2 + f32x4(-5.448, 0.6286, 33.37, -285.1) * x2 * y2;
    f32x2 AB = clamp(r.xy / r.zw, 0.0, 1.0);
    return F0 * AB.x + F90 * AB.y;
}

public func GGX_energy_compensation(f32 NdotV, f32 alpha, f32x3 Fss) -> f32x3 {
    f32 Ess = GGX_Albedo_Analytic(NdotV, alpha, 1.0, 1.0).x;
    return 1.0 + Fss * (1.0 - Ess) / Ess;
}

public func BRDF(f32x3 V, f32x3 N, f32x3 L, f32x3 albedo, f32 roughness, f32 metallic) -> f32x3 {
    f32x3 H = normalize(V + L);
    f32 NoV = abs(dot(N, V)) + 1e-5;
    f32 NoL = max(dot(N, L), 0.0);
    f32 NoH = max(dot(N, H), 0.0);
    f32 LoH = max(dot(L, H), 0.0);

    const f32 reflectance = 0.5;
    f32x3 F0 = 0.16 * reflectance * reflectance * (1.0 - metallic) + albedo * metallic;

    // Microfacet
    f32 roughness2 = roughness * roughness;
    f32 D = D_GGX(NoH, roughness2);
    f32x3 G2 = Smith_G2_Height_Correlated_GGX_Lagarde(NoV, NoL, roughness2);
    f32x3 F = F_Schlick(LoH, F0);

    f32x3 comp = GGX_energy_compensation(NoV, roughness2, F0);

    // Reflectance (Cook-Torrance)
    // V already divided by denominator
    f32x3 specular = F * D * G2;
    // Diffuse
    f32x3 diffuse = (1.0 - metallic) * albedo * Fd_Lambert();
    // Frensel combination
    return diffuse + specular * comp;
}
