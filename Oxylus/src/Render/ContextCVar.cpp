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
      self.cvar_frame_limit.set(v->get());
  }

  return true;
}
} // namespace ox
