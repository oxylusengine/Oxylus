implementing common;

public static constexpr f32 HALF_PI = 1.57079632679;
public static constexpr f32 PI = 3.1415926535897932384626433832795;
public static constexpr f32 TAU = 6.283185307179586476925286766559;

public namespace com {
/// PHASE FUNCTIONS
// Refer to https://eugenedeon.com/hitchhikers (v0.3.2)

// 4.1
//  (Howell et al., 2016, p.754) state that “In nature, however, no object scatters light isotropically”.
//  However, in 3D, spherical particles with perfect mirror surfaces produce isotropic scattering (Hauge,
//  1970) (in a far-eld (parallel) sense, assuming geometrical optics). This interesting property is not
//  true in other dimensions.

public constexpr func uniform_phase() -> f32 {
    return 1.0 / (4.0 * PI);
}

// 4.3
//  Rayleigh scattering is the primary scattering process that makes the sky blue. This is due to light
//  scattering with the air molecules. Sky models for visible light also include Mie scattering for dust
//  particles (Preetham et al., 1999; Bruneton and Neyret, 2008; Bruneton, 2016; Hillaire, 2020). Ko-
//  rkin et al. (2020) provides benchmark solutions for Rayleigh scattering in spherical atmospheres.
//  Rayleigh scattering can also approximate the scattering from cigarette smoke (Rushmeier, 1995).

public func rayleigh_phase(f32 altitude) -> f32 {
    const f32 k = 3.0 / (16.0 * PI);
    return k * (1.0 + altitude * altitude);
}

// 4.19
//  Cornette and Shanks (1992) proposed an analytic parametric phase function has been used to ap-
//  proximate Mie scattering in physically-based sky models.

public func cornette_shanks_phase(f32 g, f32 altitude) -> f32 {
    const f32 g2 = g * g;
    const f32 scale = 3.0 / (8.0 * PI);
    const f32 num = (1.0 - g2) * (1.0 + altitude * altitude);
    const f32 denom = (2.0 + g2) * pow(abs(1.0 + g2 - 2.0 * g * altitude), 1.5);

    return scale * num / denom;
}

public func henyey_greenstein_phase(f32 g, f32 cos_theta) -> f32 {
    const f32 g2 = g * g;
    const f32 denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// 4.20
//  Draine (2003) generalized the Cornette-Shanks phase function with asymmetry parameter −1 < a <
//  1 (which acts much like the mean cosine control д in Henyey-Greenstein), and an additional shape
//  control α ≥ −1. Cornette-Shanks is recovered with α = 1 and Henyey-Greenstein with α = 0. For
//  α = 1 and a = 0, Rayleigh scattering results.

public func draine_phase(f32 alpha, f32 g, f32 cos_theta) -> f32 {
    return (1.0 / (4.0 * PI)) *
           ((1.0 - (g * g)) / pow((1.0 + (g * g) - (2.0 * g * cos_theta)), 3.0 / 2.0)) *
           ((1.0 + (alpha * cos_theta * cos_theta)) / (1.0 + (alpha * (1.0 / 3.0) * (1.0 + (2.0 * g * g)))));
}

public func henyey_greenstein_draine_phase(f32 g, f32 cos_theta) -> f32 {
    const f32 g_hg = exp(-(0.0990567 / (g - 1.67154)));
    const f32 g_d = exp(-(2.20679 / (g + 3.91029)) - 0.428934);
    const f32 alpha = exp(3.62489 - (8.29288 / (g + 5.52825)));
    const f32 w_d = exp(-(0.599085 / (g - 0.641583)) - 0.665888);

    return lerp(draine_phase(0, g_hg, cos_theta), draine_phase(alpha, g_d, cos_theta), w_d);
}

/// GENERAL MATH
public func safe_sqrt(f32 x) -> f32 {
    return sqrt(max(0, x));
}

public func mod(f32x3 x, f32x3 y) -> f32x3 {
  return x - y * floor(x / y);
}

/// INTERSECTION
public func ray_sphere_intersect(f32x3 ray_origin, f32x3 ray_direction, f32 sphere_radius) -> Optional<f32x2> {
    const let a = dot(ray_direction, ray_direction);
    const let b = 2.0 * dot(ray_direction, ray_origin);
    const let c = dot(ray_origin, ray_origin) - (sphere_radius * sphere_radius);
    const let delta = b * b - 4.0 * a * c;

    if (delta < 0.0) {
        return none;
    }

    const let sqrt_delta = sqrt(delta);
    return (-b + f32x2(-1.0, 1.0) * sqrt_delta) / (2.0 * a);
}

public func ray_sphere_intersect_nearest(f32x3 ray_origin, f32x3 ray_direction, f32 sphere_radius) -> Optional<f32> {
    const let sol = ray_sphere_intersect(ray_origin, ray_direction, sphere_radius);
    if (!sol.hasValue) {
        return none;
    }

    const let sol0 = sol.value.x;
    const let sol1 = sol.value.y;

    if (sol0 < 0.0 && sol1 < 0.0) {
        return none;
    }

    if (sol0 < 0.0) {
        return max(0.0, sol1);
    } else if (sol1 < 0.0) {
        return max(0.0, sol0);
    }

    return max(0.0, min(sol0, sol1));
}

public func ray_sphere_intersect_inside(f32x3 ray_origin, f32x3 ray_direction, f32 sphere_radius) -> Optional<f32> {
    const let sol = ray_sphere_intersect(ray_origin, ray_direction, sphere_radius);
    if (!sol.hasValue) {
        return none;
    }

    return sol.value.y;
}

public func ray_sphere_intersect_outside(f32x3 ray_origin, f32x3 ray_direction, f32 sphere_radius) -> Optional<f32x2> {
    const let sol = ray_sphere_intersect(ray_origin, ray_direction, sphere_radius);
    if (!sol.hasValue) {
        return none;
    }

    const let sol0 = sol.value.x;
    const let sol1 = sol.value.y;
    if (sol1 <= 0.0 || sol0 <= 0.0) {
        return none;
    }

    return f32x2(sol0, sol1);
}

public func ray_rect_intersect(f32x2 bottom_left_0, f32x2 top_right_0, f32x2 bottom_left_1, f32x2 top_right_1) -> bool {
  return !(any(top_right_0 < bottom_left_1) || any(bottom_left_0 > top_right_1));
}

public func build_orthonormal_basis(f32x3 n) -> f32x3x3 {
    f32x3 b1;
    f32x3 b2;

    if (n.z < 0.0) {
        const f32 a = 1.0 / (1.0 - n.z);
        const f32 b = n.x * n.y * a;
        b1 = f32x3(1.0 - n.x * n.x * a, -b, n.x);
        b2 = f32x3(b, n.y * n.y * a - 1.0, -n.y);
    } else {
        const f32 a = 1.0 / (1.0 + n.z);
        const f32 b = -n.x * n.y * a;
        b1 = f32x3(1.0 - n.x * n.x * a, b, -n.x);
        b2 = f32x3(b, 1.0 - n.y * n.y * a, -n.y);
    }
    return transpose(f32x3x3(b1, b2, n));
}

public func unpack_u32_low(u32 packed) -> u32 { return packed & 0xFFFF; }
public func unpack_u32_high(u32 packed) -> u32 { return (packed >> 16) & 0xFFFF; }
}
