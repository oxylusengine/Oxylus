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

  AutoCVar_Int cvar_vsync;
  AutoCVar_Int cvar_frame_limit;
};
} // namespace ox
