#pragma once

#include <simdjson.h>

#include "Utils/CVars.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox {
struct RendererCVar {
  CVarSystem system;

  RendererCVar();

  auto init(this RendererCVar& self) -> void;

  auto to_json(this const RendererCVar& self, JsonWriter& writer) -> void;
  auto from_json(this const RendererCVar& self, simdjson::ondemand::value& json) -> void;

  AutoCVar_Int cvar_enable_debug_renderer;
  AutoCVar_Int cvar_draw_bounding_boxes;
  AutoCVar_Int cvar_enable_physics_debug_renderer;
  AutoCVar_Int cvar_freeze_culling_frustum;
  AutoCVar_Int cvar_draw_camera_frustum;
  AutoCVar_Int cvar_debug_view;
  AutoCVar_Int cvar_culling_frustum;
  AutoCVar_Int cvar_culling_occlusion;
  AutoCVar_Int cvar_culling_triangle;
  // Not serialized: describes the target being rendered into, not the scene.
  AutoCVar_Int cvar_transparent_background;

  AutoCVar_Int cvar_contact_shadows_enabled;
  AutoCVar_Int cvar_contact_shadows_steps;
  AutoCVar_Float cvar_contact_shadows_thickness;
  AutoCVar_Float cvar_contact_shadows_length;

  AutoCVar_Int cvar_vbgtao_enable;
  AutoCVar_Int cvar_vbgtao_quality_level;
  AutoCVar_Float cvar_vbgtao_thickness;
  AutoCVar_Float cvar_vbgtao_radius;
  AutoCVar_Float cvar_vbgtao_final_power;

  AutoCVar_Int cvar_rtao_enable;
  AutoCVar_Int cvar_rtao_ray_count;
  AutoCVar_Float cvar_rtao_radius;
  AutoCVar_Float cvar_rtao_power;

  AutoCVar_Int cvar_ddgi_enable;
  AutoCVar_Int cvar_ddgi_rays_per_probe;
  AutoCVar_Float cvar_ddgi_max_ray_distance;
  AutoCVar_Float cvar_ddgi_max_ray_radiance;
  AutoCVar_Int cvar_ddgi_update_max_interval;
  AutoCVar_Float cvar_ddgi_update_full_rate_distance;
  AutoCVar_Int cvar_ddgi_probe_relocation;
  AutoCVar_Float cvar_ddgi_min_frontface_distance;
  AutoCVar_Float cvar_ddgi_shadow_ray_offset;
  AutoCVar_Float cvar_ddgi_normal_bias;
  AutoCVar_Float cvar_ddgi_view_bias;
  AutoCVar_Float cvar_ddgi_hysteresis;
  AutoCVar_Float cvar_ddgi_intensity;
  AutoCVar_Float cvar_ddgi_probe_debug_radius;

  AutoCVar_Int cvar_bloom_enable;
  AutoCVar_Float cvar_bloom_threshold;
  AutoCVar_Float cvar_bloom_soft_threshold;
  AutoCVar_Float cvar_bloom_radius;
  AutoCVar_Float cvar_bloom_intensity;
  AutoCVar_Float cvar_bloom_clamp;

  AutoCVar_Int cvar_fxaa_enable;

  AutoCVar_Int cvar_tonemapper;
  AutoCVar_Float cvar_exposure;
  AutoCVar_Float cvar_gamma;
};
} // namespace ox
