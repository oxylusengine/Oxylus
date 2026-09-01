#pragma once

#include "Utils/CVars.hpp"

namespace ox {
struct ContextCVar : public CVarInterface {
  static constexpr const char* CONTEXT_CVAR_PATH = "context_config.toml";

  ContextCVar();
  ~ContextCVar();

  auto init(this ContextCVar& self) -> void;

  auto save(this ContextCVar& self) -> void;
  auto load(this ContextCVar& self) -> bool;
  auto initialize_ui_scale(this ContextCVar& self, f32 display_scale) -> void;

  AutoCVar_Int cvar_vsync;
  AutoCVar_Int cvar_frame_limit;
  AutoCVar_Float cvar_ui_scale;
  AutoCVar_Int cvar_mesh_shaders;
  AutoCVar_Int cvar_ray_tracing;

private:
  bool ui_scale_is_absolute = false;
};
} // namespace ox
