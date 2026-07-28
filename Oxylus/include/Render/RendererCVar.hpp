#pragma once

#include "Utils/CVars.hpp"
#include "Utils/JsonWriter.hpp"

#include <simdjson.h>

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

  AutoCVar_Int cvar_contact_shadows_enabled;
  AutoCVar_Int cvar_contact_shadows_steps;
  AutoCVar_Float cvar_contact_shadows_thickness;
  AutoCVar_Float cvar_contact_shadows_length;

  AutoCVar_Int cvar_vbgtao_enable;
  AutoCVar_Int cvar_vbgtao_quality_level;
  AutoCVar_Float cvar_vbgtao_thickness;
  AutoCVar_Float cvar_vbgtao_radius;
  AutoCVar_Float cvar_vbgtao_final_power;

  AutoCVar_Int cvar_bloom_enable;
  AutoCVar_Float cvar_bloom_threshold;
  AutoCVar_Float cvar_bloom_soft_threshold;
  AutoCVar_Float cvar_bloom_radius;

  AutoCVar_Int cvar_fxaa_enable;

  AutoCVar_Int cvar_tonemapper;
  AutoCVar_Float cvar_exposure;
  AutoCVar_Float cvar_gamma;
};
} // namespace ox
