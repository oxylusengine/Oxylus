// where a pixel looks when no ray found anything, shared by classify and intersect
// SSSR_CLASSIFY selects the lookup for pixels above the roughness threshold, SSSR_ENV_DDGI reads the probe
// volumes, SSSR_ENV_SKY the sky and neither falls back to flat ambient

#if !defined(SSSR_ENV_DDGI) && !defined(SSSR_ENV_SKY)
[[vk::constant_id(0)]] const u32 scene_flags = 0;
#endif

#if defined(SSSR_ENV_DDGI) || defined(SSSR_ENV_SKY)
[[vk::binding(20, 0)]] Sampler environment_sampler;
#endif

#if defined(SSSR_ENV_DDGI)
[[vk::binding(21, 0)]] StructuredBuffer<ProbeVolume> probe_volumes;
[[vk::binding(22, 0)]] StructuredBuffer<ProbeState> probe_states;
[[vk::binding(23, 0)]] Texture2D<f32x4> irradiance_atlas;
[[vk::binding(24, 0)]] Texture2D<f32x2> distance_atlas;
#endif

#if defined(SSSR_CLASSIFY)
    #if defined(SSSR_ENV_SKY)
[[vk::binding(21, 0)]] TextureCube<f32x4> sky_cubemap;
    #endif

func sssr_rough_environment(
    in SSSRSettings settings,
    f32x3 world_position,
    f32x3 N,
    f32x3 geometric_N,
    f32x3 V,
    f32 view_distance,
    f32x3 R,
    f32 perceptual_roughness
) -> f32x3 {
    #if defined(SSSR_ENV_DDGI)
    ProbeSampleParams params;
    params.world_position = world_position;
    params.normal = N;
    params.geometric_normal = geometric_N;
    params.view_direction = V;
    params.view_distance = view_distance;
    params.specular_direction = normalize(lerp(N, R, 1.0 - perceptual_roughness));
    params.want_specular = true;
    params.normal_bias = settings.ddgi_normal_bias;
    params.view_bias = settings.ddgi_view_bias;

    let sample = sample_ddgi_irradiance(
        probe_volumes,
        probe_states,
        settings.ddgi_volume_count,
        irradiance_atlas,
        distance_atlas,
        environment_sampler,
        params
    );

    return lerp(settings.sky_ambient_color, sample.specular * settings.ddgi_intensity, sample.coverage);
    #elif defined(SSSR_ENV_SKY)
    return sky_cubemap_specular(sky_cubemap, environment_sampler, N, R, perceptual_roughness, settings.horizon_darkening);
    #else
    let has_sky = (scene_flags & SceneFlags::HasSky) != 0;
    return has_sky ? settings.sky_ambient_color : settings.base_ambient_color;
    #endif
}
#else
    #if defined(SSSR_ENV_SKY)
[[vk::binding(25, 0)]] Texture2D<f32x4> sky_view_lut;
    #endif

// a ray that misses must not leak light into corners, and one that hits a lit pixel must stay undarkened, so
// the occlusion scales the fallback only
func sssr_ray_environment(
    in SSSRSettings settings,
    #if defined(SSSR_ENV_SKY)
    Atmosphere *atmosphere,
    #endif
    f32x3 world_position,
    f32x3 N,
    f32x3 geometric_N,
    f32x3 V,
    f32 view_distance,
    f32x3 direction,
    bool is_sky,
    i32x2 texel
) -> f32x3 {
    f32x3 radiance;
    #if defined(SSSR_ENV_DDGI)
        #if defined(SSSR_ENV_SKY)
    if (is_sky) {
        radiance = get_atmosphere_illuminance_along_ray(
            atmosphere,
            environment_sampler,
            sky_view_lut,
            settings.sun_direction,
            atmosphere.get_eye_pos(camera.position.xyz),
            direction
        );
    } else
        #endif
    {
        ProbeSampleParams params;
        params.world_position = world_position;
        params.normal = N;
        params.geometric_normal = geometric_N;
        params.view_direction = V;
        params.view_distance = view_distance;
        params.specular_direction = direction;
        params.want_specular = true;
        params.normal_bias = settings.ddgi_normal_bias;
        params.view_bias = settings.ddgi_view_bias;

        let sample = sample_ddgi_irradiance(
            probe_volumes,
            probe_states,
            settings.ddgi_volume_count,
            irradiance_atlas,
            distance_atlas,
            environment_sampler,
            params
        );

        radiance = lerp(settings.sky_ambient_color, sample.specular * settings.ddgi_intensity, sample.coverage);
    }
    #elif defined(SSSR_ENV_SKY)
    radiance = get_atmosphere_illuminance_along_ray(
        atmosphere,
        environment_sampler,
        sky_view_lut,
        settings.sun_direction,
        atmosphere.get_eye_pos(camera.position.xyz),
        direction
    );
    #else
    let has_sky = (scene_flags & SceneFlags::HasSky) != 0;
    radiance = has_sky ? settings.sky_ambient_color : settings.base_ambient_color;
    #endif

    return radiance * specular_occlusion_image.Load(i32x3(texel, 0));
}
#endif
