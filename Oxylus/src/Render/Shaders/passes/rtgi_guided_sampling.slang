// textually included by rtgi_trace.slang

// von Mises-Fisher kappa from the mean resultant length, the clamp is a float safety margin
func rtgi_concentration_to_kappa(f32 concentration) -> f32 {
    let rc = clamp(concentration, 0.0, 0.999);
    return rc * (3.0 - rc * rc) / max(1.0 - rc * rc, 1.0e-4);
}

func rtgi_schlick_pull(f32 u, f32 c) -> f32 {
    return u / (1.0 + c * (1.0 - u));
}

func rtgi_schlick_pull_pdf(f32 u_prime, f32 c) -> f32 {
    let d = 1.0 + c * u_prime;
    return (1.0 + c) / (d * d);
}

func rtgi_disc_project(com::TangentBasis basis, f32x3 dir) -> f32x2 {
    return basis.to_local(dir).xy;
}

// ray circle intersection from the interior point pd, |pd + t * dir2| = 1 solved for t > 0
func rtgi_disc_boundary_t_max(f32x2 dir2, f32x2 pd) -> f32 {
    let b = dot(pd, dir2);
    let c = 1.0 - dot(pd, pd);
    return -b + sqrt(max(0.0, b * b + c));
}

// theta comes from an origin centred uniform disc point, its s0^2 around pd is exactly uniform and independent of theta
func rtgi_sample_disc_warp_dir(com::TangentBasis basis, f32x2 pd, f32 c) -> f32x3 {
    let u1 = com::rand();
    let u2 = com::rand();
    let r0 = sqrt(u1);
    let phi0 = 2.0 * PI * u2;
    let p0 = f32x2(r0 * cos(phi0), r0 * sin(phi0));

    let delta = p0 - pd;
    let rho0 = length(delta);
    let theta = atan2(delta.y, delta.x);
    let t_max = rtgi_disc_boundary_t_max(f32x2(cos(theta), sin(theta)), pd);
    let s0 = rho0 / max(t_max, 1.0e-6);
    let s0_sq = select(t_max > 1.0e-6, min(s0 * s0, 1.0 - 1.0e-6), 0.0);

    let pulled = rtgi_schlick_pull(s0_sq, c);
    let s = sqrt(pulled);
    let rho = s * t_max;
    let p = pd + rho * f32x2(cos(theta), sin(theta));
    let z = sqrt(max(0.0, 1.0 - dot(p, p)));
    return basis.to_world(f32x3(p, z));
}

func rtgi_disc_warp_pdf(com::TangentBasis basis, f32x2 pd, f32 c, f32x3 dir) -> f32 {
    let local = basis.to_local(dir);
    let delta = local.xy - pd;
    let rho = length(delta);
    let dir2 = select(rho > 1.0e-6, delta / max(rho, 1.0e-6), f32x2(1.0, 0.0));
    let t_max = rtgi_disc_boundary_t_max(dir2, pd);
    let s2 = min((rho / t_max) * (rho / t_max), 1.0 - 1.0e-6);

    let pull_pdf = rtgi_schlick_pull_pdf(s2, c);
    return select(local.z <= 0.0, 0.0, pull_pdf / PI * local.z);
}

// the caller multiplies the traced radiance by weight, at concentration 0 it is exactly 1
func rtgi_sample_guided_diffuse_dir(f32x3 normal, f32x3 guide_direction, f32 concentration, out f32 weight) -> f32x3 {
    let c = rtgi_concentration_to_kappa(concentration);
    let basis = com::build_tangent_basis(normal);
    let pd = rtgi_disc_project(basis, guide_direction);

    let dir = rtgi_sample_disc_warp_dir(basis, pd, c);

    let pdf_cosine = com::cosine_hemi_pdf(normal, dir);
    let pdf_guide = rtgi_disc_warp_pdf(basis, pd, c, dir);
    weight = select(pdf_guide > 1.0e-8, pdf_cosine / max(pdf_guide, 1.0e-8), 0.0);
    return dir;
}
