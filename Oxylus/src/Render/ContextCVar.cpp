#include "Render/ContextCVar.hpp"

#include <toml++/toml.hpp>

#include "OS/File.hpp"

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
  self.cvar_bindless_descriptor_count
    .init(self.system, "rr.bindless_descriptor_count", "Requested capacity for each bindless descriptor array", 65536);
  self.cvar_mesh_shaders
    .init(self.system, "rr.mesh_shaders", "Use the mesh shader geometry pipeline when the device supports it", 1);
  self.cvar_ray_tracing
    .init(self.system, "rr.ray_tracing", "Build acceleration structures when the device supports them", 1);
}

auto ContextCVar::save(this ContextCVar& self) -> void {
  ZoneScoped;

  auto root = toml::table{
    {
      "display",
      toml::table{
        {"vsync", (bool)self.cvar_vsync.get()},
        {"frame_limit", self.cvar_frame_limit.get()},
      },
    },
    {
      "render",
      toml::table{
        {"bindless_descriptor_count", self.cvar_bindless_descriptor_count.get()},
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
  }

  if (const auto render_config = toml["render"]) {
    if (auto v = render_config["bindless_descriptor_count"].as_integer())
      self.cvar_bindless_descriptor_count.set(static_cast<i32>(v->get()));
    if (auto v = render_config["mesh_shaders"].as_boolean())
      self.cvar_mesh_shaders.set(v->get());
    if (auto v = render_config["ray_tracing"].as_boolean())
      self.cvar_ray_tracing.set(v->get());
  }

  return true;
}
} // namespace ox
