#pragma once

// Keep this header private in src

#include <flecs.h>

#include "Scene/Components.hpp"

namespace ox {
struct RendererInstanceQueries {
  flecs::query<const TransformComponent, const CameraComponent> cameras;
  flecs::query<const TransformComponent, const LightComponent> lights;
  flecs::query<const AtmosphereComponent> atmospheres;
  flecs::query<const SkyComponent> skies;
  flecs::query<const TransformComponent, const HeightFogComponent> height_fogs;
  flecs::query<const TransformComponent, const FogVolumeComponent> fog_volumes;
  flecs::query<const TransformComponent, const ProbeVolumeComponent> probe_volumes;
  flecs::query<const TransformComponent, const DecalComponent> decals;
  flecs::query<const TransformComponent, const SpriteComponent> sprites;
  flecs::query<const TransformComponent, const ParticleSystemComponent> particle_systems;
  flecs::query<const AutoExposureComponent> auto_exposures;
  flecs::query<const TransformComponent, const VignetteComponent> vignettes;
  flecs::query<const TransformComponent, const ChromaticAberrationComponent> chromatic_aberrations;
  flecs::query<const TransformComponent, const FilmGrainComponent> film_grains;
  flecs::query<const TransformComponent, const LetterboxComponent> letterboxes;
  flecs::query<const TransformComponent, const ScreenFadeComponent> screen_fades;
  flecs::query<const TonemappingComponent> tonemappings;
  flecs::query<const ColorGradingComponent> color_gradings;

  static auto create(flecs::world& world) -> std::unique_ptr<RendererInstanceQueries> {
    auto queries = std::make_unique<RendererInstanceQueries>();
    queries->cameras = world.query_builder<const TransformComponent, const CameraComponent>().cached().build();
    queries->lights = world.query_builder<const TransformComponent, const LightComponent>().cached().build();
    queries->atmospheres = world.query_builder<const AtmosphereComponent>().cached().build();
    queries->skies = world.query_builder<const SkyComponent>().cached().build();
    queries->height_fogs = world.query_builder<const TransformComponent, const HeightFogComponent>().cached().build();
    queries->fog_volumes = world.query_builder<const TransformComponent, const FogVolumeComponent>().cached().build();
    queries
      ->probe_volumes = world.query_builder<const TransformComponent, const ProbeVolumeComponent>().cached().build();
    queries->decals = world.query_builder<const TransformComponent, const DecalComponent>().cached().build();
    queries->sprites = world.query_builder<const TransformComponent, const SpriteComponent>().cached().build();
    queries->particle_systems = world.query_builder<const TransformComponent, const ParticleSystemComponent>()
                                  .cached()
                                  .build();
    queries->auto_exposures = world.query_builder<const AutoExposureComponent>().cached().build();
    queries->vignettes = world.query_builder<const TransformComponent, const VignetteComponent>().cached().build();
    queries->chromatic_aberrations = world.query_builder<const TransformComponent, const ChromaticAberrationComponent>()
                                       .cached()
                                       .build();
    queries->film_grains = world.query_builder<const TransformComponent, const FilmGrainComponent>().cached().build();
    queries->letterboxes = world.query_builder<const TransformComponent, const LetterboxComponent>().cached().build();
    queries->screen_fades = world.query_builder<const TransformComponent, const ScreenFadeComponent>().cached().build();
    queries->tonemappings = world.query_builder<const TonemappingComponent>().cached().build();
    queries->color_gradings = world.query_builder<const ColorGradingComponent>().cached().build();
    return queries;
  }
};
} // namespace ox
