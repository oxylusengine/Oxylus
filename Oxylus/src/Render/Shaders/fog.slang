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

func no_fog() -> FogSample {
    FogSample result;
    result.inscatter = f32x3(0.0);
    result.transmittance = 1.0;
    return result;
}

// normalized over the sphere, so an isotropic medium scatters 1 / 4pi of the light toward the eye
public func henyey_greenstein(f32 cos_theta, f32 g) -> f32 {
    let g2 = g * g;
    let denominator = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * PI * denominator * sqrt(denominator));
}

// density is fog.density * exp(-falloff * (y - base)) and integrates in closed form along a straight ray
func height_fog_optical_depth(in HeightFog fog, f32x3 origin, f32x3 direction, f32 distance, f32 min_start) -> f32 {
    let start = min(max(fog.start_distance, min_start), distance);
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

// the analytic fog from `min_start` along the ray on, where the froxels hand over to it
func evaluate_height_fog_from(
    in HeightFog fog, f32x3 camera_position, f32x3 world_position, f32 min_start, f32x3 ambient_radiance
) -> FogSample {
    if (fog.density <= 0.0) {
        return no_fog();
    }

    let to_point = world_position - camera_position;
    let distance = length(to_point);
    if (distance <= 1e-4) {
        return no_fog();
    }

    let direction = to_point / distance;
    let optical_depth = height_fog_optical_depth(fog, camera_position, direction, distance, min_start);
    let opacity = min(1.0 - exp(-optical_depth), fog.max_opacity);
    let phase = henyey_greenstein(dot(direction, fog.sun_direction), fog.anisotropy);

    FogSample result;
    result.inscatter = (ambient_radiance + fog.sun_radiance * phase) * opacity;
    result.transmittance = 1.0 - opacity;
    return result;
}

public func evaluate_height_fog(in HeightFog fog, f32x3 camera_position, f32x3 world_position) -> FogSample {
    return evaluate_height_fog_from(fog, camera_position, world_position, 0.0, fog.ambient_radiance);
}

// the froxel slices are spaced exponentially in view depth between volumetric_near and volumetric_distance
public func froxel_view_depth(in HeightFog fog, f32 w) -> f32 {
    return fog.volumetric_near * pow(fog.volumetric_distance / fog.volumetric_near, w);
}

public func froxel_w(in HeightFog fog, f32 view_depth) -> f32 {
    return log(max(view_depth, 1e-4) / fog.volumetric_near) / log(fog.volumetric_distance / fog.volumetric_near);
}

// the grid rounds the screen up to whole tiles, so screen uv only reaches part of it
public func froxel_uv_scale(f32x2 resolution) -> f32x2 {
    let tile_size = f32(FROXEL_TILE_SIZE);
    return resolution / (ceil(resolution / tile_size) * tile_size);
}

// the froxels out to volumetric_distance, then the analytic fog for the rest of the ray. Without volumetric fog it's the
// analytic fog alone and the froxels are never read
public func evaluate_fog(
    in Camera camera, f32x3 world_position, f32x2 screen_uv, Texture3D<f32x4> froxels, SamplerState froxel_sampler
) -> FogSample {
    let fog = camera.fog;
    let camera_position = camera.position.xyz;
    if (fog.volumetric_distance <= 0.0) {
        return evaluate_height_fog(fog, camera_position, world_position);
    }

    let to_point = world_position - camera_position;
    let direction = to_point / max(length(to_point), 1e-4);
    let cos_forward = max(dot(direction, camera.forward), 1e-4);

    // integrated texel z holds the fog out to the far edge of slice z, so a point at slice coordinate s reads texel s - 1.
    // The texel past the last slice holds the column's far ambient, so the lookup stops short of it
    let slices = f32(FROXEL_DEPTH_SLICES);
    let texels = f32(FROXEL_DEPTH_TEXELS);
    let froxel_uv = screen_uv * froxel_uv_scale(camera.resolution);
    let texel = clamp(froxel_w(fog, dot(to_point, camera.forward)) * slices - 1.0, 0.0, slices - 1.0);
    let froxel = froxels.SampleLevel(froxel_sampler, f32x3(froxel_uv, (texel + 0.5) / texels), 0.0);
    let far_ambient = froxels.SampleLevel(froxel_sampler, f32x3(froxel_uv, (slices + 0.5) / texels), 0.0).rgb;
    let beyond = evaluate_height_fog_from(
        fog, camera_position, world_position, fog.volumetric_distance / cos_forward, far_ambient);

    FogSample result;
    result.inscatter = froxel.rgb + froxel.a * beyond.inscatter;
    result.transmittance = froxel.a * beyond.transmittance;

    // max_opacity caps the whole column, froxels and analytic together
    let opacity = 1.0 - result.transmittance;
    if (opacity > fog.max_opacity) {
        result.inscatter *= fog.max_opacity / opacity;
        result.transmittance = 1.0 - fog.max_opacity;
    }

    return result;
}

public func evaluate_fog_sky(
    in Camera camera, f32x3 direction, f32x2 screen_uv, Texture3D<f32x4> froxels, SamplerState froxel_sampler
) -> FogSample {
    let fog = camera.fog;
    var distance = fog.sky_distance;
    if (fog.volumetric_distance > 0.0) {
        // past the last slice, so the sky takes the whole froxel column even when sky_distance leaves it clear
        distance = max(distance, 1.001 * fog.volumetric_distance / max(dot(direction, camera.forward), 1e-4));
    }

    if (distance <= 0.0) {
        return no_fog();
    }

    return evaluate_fog(camera, camera.position.xyz + direction * distance, screen_uv, froxels, froxel_sampler);
}
