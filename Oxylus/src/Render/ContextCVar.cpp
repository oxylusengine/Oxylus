#include "Render/ContextCVar.hpp"

#include <toml++/toml.hpp>

#include "OS/File.hpp"
#include "UI/UIScale.hpp"

namespace ox {
ContextCVar::ContextCVar() {
  ZoneScoped;

  init();
  load();
}

ContextCVar::~ContextCVar() {
  ZoneScoped;

  save();
}

auto ContextCVar::init(this ContextCVar& self) -> void {
  ZoneScoped;

  self.cvar_vsync.init(self.system, "rr.vsync", "toggle vsync", 1);
  self.cvar_frame_limit
    .init(self.system, "rr.frame_limit", "Limits the framerate with a sleep. 0: Disable, > 0: Enable", 0);
  self.cvar_ui_scale.init(self.system, "ui.scale", "UI scale multiplier", UI_SCALE_DEFAULT_MULTIPLIER);
  self.cvar_mesh_shaders
    .init(self.system, "rr.mesh_shaders", "Use the mesh shader geometry pipeline when the device supports it", 1);
  self.cvar_ray_tracing
    .init(self.system, "rr.ray_tracing", "Build acceleration structures when the device supports them", 1);
}

auto ContextCVar::save(this ContextCVar& self) -> void {
  ZoneScoped;

  const auto ui_scale = normalize_ui_scale_multiplier(self.cvar_ui_scale.get());
  self.cvar_ui_scale.set(ui_scale);

  auto root = toml::table{
    {
      "display",
      toml::table{
        {"vsync", (bool)self.cvar_vsync.get()},
        {"frame_limit", self.cvar_frame_limit.get()},
        {"ui_scale", ui_scale},
        {"ui_scale_is_absolute", self.ui_scale_is_absolute},
      },
    },
    {
      "render",
      toml::table{
        {"mesh_shaders", (bool)self.cvar_mesh_shaders.get()},
        {"ray_tracing", (bool)self.cvar_ray_tracing.get()},
      },
    },
  };

  std::stringstream ss;
  ss << root;
  auto file = File(CONTEXT_CVAR_PATH, FileAccess::Write);
  file.write(ss.str());
}

auto ContextCVar::load(this ContextCVar& self) -> bool {
  ZoneScoped;

  auto content = File::to_string(CONTEXT_CVAR_PATH);
  if (content.empty())
    return false;

  toml::table toml = toml::parse(content);

  if (const auto display_config = toml["display"]) {
    if (auto v = display_config["vsync"].as_boolean())
      self.cvar_vsync.set(v->get());
    if (auto v = display_config["frame_limit"].as_integer())
      self.cvar_frame_limit.set(static_cast<i32>(v->get()));
    if (auto floating_scale = display_config["ui_scale"].as_floating_point())
      self.cvar_ui_scale.set(normalize_ui_scale_multiplier(static_cast<f32>(floating_scale->get())));
    else if (auto integer_scale = display_config["ui_scale"].as_integer())
      self.cvar_ui_scale.set(normalize_ui_scale_multiplier(static_cast<f32>(integer_scale->get())));
    if (auto absolute_scale = display_config["ui_scale_is_absolute"].as_boolean())
      self.ui_scale_is_absolute = absolute_scale->get();
  }

  if (const auto render_config = toml["render"]) {
    if (auto v = render_config["mesh_shaders"].as_boolean())
      self.cvar_mesh_shaders.set(v->get());
    if (auto v = render_config["ray_tracing"].as_boolean())
      self.cvar_ray_tracing.set(v->get());
  }

  return true;
}

auto ContextCVar::initialize_ui_scale(this ContextCVar& self, f32 display_scale) -> void {
  ZoneScoped;

  if (self.ui_scale_is_absolute) {
    return;
  }

  self.cvar_ui_scale.set(migrate_legacy_ui_scale(display_scale, self.cvar_ui_scale.get()));
  self.ui_scale_is_absolute = true;
}
} // namespace ox
