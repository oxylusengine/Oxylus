import common;
import gpu;
import scene;
import lighting;
import fog;
import ddgi;

[[vk::constant_id(0)]] const u32 scene_flags = 0;
[[vk::constant_id(1)]] const u32 has_directional_shadows = 0;

[[vk::binding(0, 0)]] ConstantBuffer<Camera> camera;
[[vk::binding(1, 0)]] ConstantBuffer<VolumetricFogParams> params;
[[vk::binding(2, 0)]] SamplerState nearest_clamp_sampler;
[[vk::binding(3, 0)]] SamplerState linear_clamp_sampler;
[[vk::binding(4, 0)]] Texture2DArray<u32> vsm_page_table;
[[vk::binding(5, 0)]] Texture2DArray<u32> vsm_pointspot_page_tables;
[[vk::binding(6, 0)]] Texture2D<f32> vsm_physical_pages;
[[vk::binding(7, 0)]] Texture3D<f32x4> previous_history;
[[vk::binding(8, 0)]] StorageImage3D<f32x4, ImageFormat::RGBA16F> history;
[[vk::binding(9, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(10, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(11, 0)]] Texture2D<f32x4> irradiance_atlas;
[[vk::binding(12, 0)]] Texture2D<f32x2> distance_atlas;
[[vk::binding(13, 0)]] TextureCube<f32x4> sky_cubemap;

struct InjectPush {
    PBRLighting *lighting;
    Light *lights;
    LightGridCell *light_grid;
    VSMPointSpotView *vsm_pointspot_views;
    VirtualClipmap *vsm_clipmaps;
    FogVolume *fog_volumes;
    // DDGIApplyPush, all zero when the frame has no probes
    u32 probe_volume_count;
    f32 ddgi_normal_bias;
    f32 ddgi_view_bias;
    f32 ddgi_intensity;
    f32x3 ddgi_ambient_color;
};
[[vk::push_constant]] InjectPush push;

func fog_noise_hash(i32x3 cell) -> u32 {
    // wrapped so the lattice repeats every FOG_NOISE_PERIOD cells, which is what lets the drift offset wrap too
    let wrapped = u32x3(cell) & (FOG_NOISE_PERIOD - 1u);
    var h = wrapped.x * 73856093u ^ wrapped.y * 19349663u ^ wrapped.z * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

func fog_noise_gradient(i32x3 cell) -> f32x3 {
    let h = fog_noise_hash(cell);
    return f32x3(f32(h & 1023u), f32((h >> 10) & 1023u), f32((h >> 20) & 1023u)) / 511.5 - 1.0;
}

// gradient noise, roughly in [-1, 1]
func fog_gradient_noise(f32x3 p) -> f32 {
    let cell = i32x3(floor(p));
    let f = p - floor(p);
    let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    var result = 0.0;
    [ForceUnroll]
    for (u32 corner = 0; corner < 8; corner++) {
        let offset = i32x3(i32(corner & 1u), i32((corner >> 1) & 1u), i32((corner >> 2) & 1u));
        let ramp = dot(fog_noise_gradient(cell + offset), f - f32x3(offset));
        let weight = select(offset.x != 0, u.x, 1.0 - u.x) * select(offset.y != 0, u.y, 1.0 - u.y) *
                     select(offset.z != 0, u.z, 1.0 - u.z);
        result += ramp * weight;
    }

    return result * 1.6;
}

// in [0, 1]. Each octave doubles the frequency exactly, so every one repeats on the same period as the drift
func fog_volume_noise(f32x3 p) -> f32 {
    var sum = 0.0;
    var amplitude = 0.5;
    var frequency = 1.0;
    [ForceUnroll]
    for (u32 octave = 0; octave < 3; octave++) {
        sum += fog_gradient_noise(p * frequency + f32(octave) * 17.31) * amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }

    return saturate(sum / 0.875 * 0.5 + 0.5);
}

// the light arriving from every direction, which an isotropic medium scatters toward the eye. Probes record
// irradiance about a normal, so the two opposite hemispheres together stand in for the whole sphere
func fog_ambient(f32x3 world_position, f32x3 ray, f32 view_distance) -> f32x3 {
    var sky = params.sky_ambient;
    if ((scene_flags & SceneFlags::HasAtmosphere) != 0) {
        let up = sky_cubemap.SampleLevel(linear_clamp_sampler, f32x3(0.0, 1.0, 0.0), 0.0);
        let down = sky_cubemap.SampleLevel(linear_clamp_sampler, f32x3(0.0, -1.0, 0.0), 0.0);
        sky = 0.5 * (up.rgb * up.a + down.rgb * down.a) * params.horizon_darkening;
    }

    if ((scene_flags & SceneFlags::HasDDGI) == 0 || push.probe_volume_count == 0) {
        return sky;
    }

    ProbeSampleParams probe;
    probe.world_position = world_position;
    probe.view_direction = -ray;
    probe.view_distance = view_distance;
    probe.want_specular = false;
    // a point in the air has no surface to push away from, only the camera's side to lean toward
    probe.normal_bias = 0.0;
    probe.view_bias = push.ddgi_view_bias;

    var irradiance = f32x3(0.0);
    var coverage = 0.0;
    [ForceUnroll]
    for (u32 side = 0; side < 2; side++) {
        let normal = f32x3(0.0, side == 0 ? 1.0 : -1.0, 0.0);
        probe.normal = normal;
        probe.geometric_normal = normal;
        probe.specular_direction = normal;
        let probe_sample = sample_ddgi_irradiance(
            probe_volumes, probe_states, push.probe_volume_count, irradiance_atlas, distance_atlas, linear_clamp_sampler, probe);
        irradiance += 0.5 * probe_sample.irradiance;
        coverage += 0.5 * probe_sample.coverage;
    }

    return lerp(sky, irradiance * push.ddgi_intensity, coverage);
}

// rgb is the light the medium adds per meter, a its extinction per meter, at one jittered spot in the froxel
[[shader("compute")]]
[[numthreads(8, 8, 1)]]
func cs_main(u32x3 id : SV_DispatchThreadID) -> void {
    if (any(id >= params.grid_size)) {
        return;
    }

    let fog = camera.fog;
    let camera_position = camera.position.xyz;
    let world_position = froxel_world_position(camera, f32x3(id) + fog.volumetric_jitter, params.grid_size);
    let ray = normalize(world_position - camera_position);

    var extinction = 0.0;
    var scattering = f32x3(0.0);
    var emission = f32x3(0.0);
    if (fog.density > 0.0 && length(world_position - camera_position) >= fog.start_distance) {
        let density = fog.density * exp(min(-fog.height_falloff * (world_position.y - fog.base_height), 80.0));
        extinction += density;
        scattering += density * params.height_albedo;
    }

    for (u32 i = 0; i < params.fog_volume_count; i++) {
        let volume = push.fog_volumes[i];
        let local = mul(volume.world_to_volume, f32x4(world_position, 1.0)).xyz;
        let face_distance = 0.5 - abs(local);
        if (any(face_distance < 0.0)) {
            continue;
        }

        let inner = min(face_distance.x, min(face_distance.y, face_distance.z));
        var fade = volume.edge_fade > 0.0 ? saturate(inner / volume.edge_fade) : 1.0;
        if (volume.noise_frequency > 0.0 && volume.noise_strength > 0.0) {
            // world space, so scaling the box grows it without stretching the wisps
            let noise = fog_volume_noise(world_position * volume.noise_frequency + volume.noise_offset);
            // the fbm clusters around 0.5, these edges leave about a third of a fully carved box empty
            fade *= lerp(1.0, smoothstep(0.45, 0.58, noise), volume.noise_strength);
        }
        extinction += volume.density * fade;
        scattering += volume.density * fade * volume.albedo;
        emission += volume.emissive * fade;
    }

    var current = f32x4(emission, extinction);
    if (any(scattering > 0.0)) {
        let resources = LightingResources(
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

        let view_distance = length(world_position - camera_position);
        var radiance = fog_ambient(world_position, ray, view_distance) * params.ambient_intensity;
        let has_directional_light = (scene_flags & SceneFlags::HasDirectionalLight) != 0;
        if (has_directional_light && any(params.sun_illuminance > 0.0)) {
            let visibility = lighting_directional_volume_visibility(
                resources, scene_flags, has_directional_shadows, world_position);
            radiance += params.sun_illuminance * visibility * henyey_greenstein(dot(ray, fog.sun_direction), fog.anisotropy);
        }

        if (params.local_light_scattering > 0.0) {
            radiance += params.local_light_scattering * lighting_point_spot_volume(resources, world_position, ray, fog.anisotropy);
        }

        current.rgb += scattering * radiance;
    }

    // the history holds the running average of every jittered spot, reprojected from where the froxel's center was last
    // frame. Not this frame's spot, it lands between texels and the fetch would blend in a different share of the
    // neighbouring slices and tiles every frame, even with the camera still
    if (params.is_history_valid != 0) {
        let center_position = froxel_world_position(camera, f32x3(id) + 0.5, params.grid_size);
        let previous_clip = mul(camera.previous_projection_view, f32x4(center_position, 1.0));
        if (previous_clip.w > 0.0) {
            let previous_screen_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
            let previous_view_depth = -mul(camera.previous_view, f32x4(center_position, 1.0)).z;
            // texel centers sit at slice centers, the extra texel past the last slice isn't part of the volume
            let depth_coordinate = froxel_w(fog, previous_view_depth) * f32(FROXEL_DEPTH_SLICES) / f32(FROXEL_DEPTH_TEXELS);
            let previous_uvw = f32x3(previous_screen_uv * froxel_uv_scale(camera.resolution), depth_coordinate);
            let max_depth_coordinate = (f32(FROXEL_DEPTH_SLICES) - 0.5) / f32(FROXEL_DEPTH_TEXELS);
            if (all(previous_uvw.xy >= 0.0) && all(previous_uvw.xy <= 1.0) && previous_uvw.z >= 0.0 &&
                previous_uvw.z <= max_depth_coordinate) {
                let previous = previous_history.SampleLevel(linear_clamp_sampler, previous_uvw, 0.0);
                current = lerp(current, previous, params.history_weight);
            }
        }
    }

    history.Store(id, current);

    // the analytic fog past the froxels takes its ambient from here, so nothing steps where the froxels end. Blended
    // in place, the column's far end barely moves as the camera does
    if (id.z == params.grid_size.z - 1u) {
        let far_depth = fog.volumetric_distance;
        let far_position = camera_position + ray * (far_depth / max(dot(ray, camera.forward), 1e-4));
        var far_ambient = fog_ambient(far_position, ray, length(far_position - camera_position)) * params.ambient_intensity *
                          params.height_albedo;
        let far_texel = u32x3(id.xy, FROXEL_DEPTH_SLICES);
        if (params.is_history_valid != 0) {
            far_ambient = lerp(far_ambient, previous_history.Load(i32x4(i32x3(far_texel), 0)).rgb, params.history_weight);
        }

        history.Store(far_texel, f32x4(far_ambient, 1.0));
    }
}
