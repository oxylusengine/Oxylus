module fog;

import common;
import gpu;
import scene;

public struct FogSample {
    // already weighted by how much of the view the fog covers
    public f32x3 inscatter;
    public f32 transmittance;

    public func apply(f32x3 color) -> f32x3 {
        return color * this.transmittance + this.inscatter;
    }
};

// normalized over the sphere, so an isotropic medium scatters 1 / 4pi of the light toward the eye
func henyey_greenstein(f32 cos_theta, f32 g) -> f32 {
    let g2 = g * g;
    let denominator = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * PI * denominator * sqrt(denominator));
}

// density is fog.density * exp(-falloff * (y - base)) and integrates in closed form along a straight ray
func height_fog_optical_depth(in HeightFog fog, f32x3 origin, f32x3 direction, f32 distance) -> f32 {
    let start = min(fog.start_distance, distance);
    let length = distance - start;
    if (length <= 0.0) {
        return 0.0;
    }

    // only the growing exponentials are clamped, a ray deep below the base or heading down a long way stays finite
    let height = origin.y + direction.y * start - fog.base_height;
    let start_density = fog.density * exp(min(-fog.height_falloff * height, 80.0));
    let falloff = fog.height_falloff * direction.y * length;
    let integral = abs(falloff) > 1e-3 ? (1.0 - exp(min(-falloff, 80.0))) / falloff : 1.0 - 0.5 * falloff;

    return start_density * length * integral;
}

public func evaluate_height_fog(in HeightFog fog, f32x3 camera_position, f32x3 world_position) -> FogSample {
    FogSample result;
    result.inscatter = f32x3(0.0);
    result.transmittance = 1.0;
    if (fog.density <= 0.0) {
        return result;
    }

    let to_point = world_position - camera_position;
    let distance = length(to_point);
    if (distance <= 1e-4) {
        return result;
    }

    let direction = to_point / distance;
    let opacity = min(1.0 - exp(-height_fog_optical_depth(fog, camera_position, direction, distance)), fog.max_opacity);
    let phase = henyey_greenstein(dot(direction, fog.sun_direction), fog.anisotropy);
    result.inscatter = (fog.ambient_radiance + fog.sun_radiance * phase) * opacity;
    result.transmittance = 1.0 - opacity;

    return result;
}

public func evaluate_height_fog_sky(in HeightFog fog, f32x3 camera_position, f32x3 direction) -> FogSample {
    if (fog.sky_distance <= 0.0) {
        FogSample result;
        result.inscatter = f32x3(0.0);
        result.transmittance = 1.0;
        return result;
    }

    return evaluate_height_fog(fog, camera_position, camera_position + direction * fog.sky_distance);
}
