#include "Render/RendererConfig.hpp"

#include <toml++/toml.hpp>

#include "OS/File.hpp"

namespace ox {

auto RendererConfig::init() -> std::expected<void, std::string> {
  if (!load_config("renderer_config.toml"))
    if (!save_config("renderer_config.toml"))
      return std::unexpected{"Couldn't load/save renderer_config.toml"};

  return {};
}

auto RendererConfig::deinit() -> std::expected<void, std::string> {
  if (!save_config("renderer_config.toml"))
    return std::unexpected{"Couldn't save renderer_config.toml"};

  return {};
}

bool RendererConfig::save_config(const std::filesystem::path& path) const {
  ZoneScoped;

  auto root = toml::table{
    {
      "display",
      toml::table{
        {"vsync", (bool)RendererCVar::cvar_vsync.get()},
      },
    },
    {
      "debug",
      toml::table{
        {"debug_renderer", (bool)RendererCVar::cvar_enable_debug_renderer.get()},
        {"bounding_boxes", (bool)RendererCVar::cvar_draw_bounding_boxes.get()},
        {"physics_debug_renderer", (bool)RendererCVar::cvar_enable_physics_debug_renderer.get()},
      },
    },
    {"color",
     toml::table{
       {"tonemapper", RendererCVar::cvar_tonemapper.get()},
       {"exposure", RendererCVar::cvar_exposure.get()},
       {"gamma", RendererCVar::cvar_gamma.get()}
     }},
    {
      "gtao",
      toml::table{
        {"enabled", (bool)RendererCVar::cvar_vbgtao_enable.get()},
        {"radius", RendererCVar::cvar_vbgtao_radius.get()},
        {"quality_level", RendererCVar::cvar_vbgtao_quality_level.get()},
      },
    },
    {
      "bloom",
      toml::table{
        {"enabled", (bool)RendererCVar::cvar_bloom_enable.get()},
        {"threshold", RendererCVar::cvar_bloom_threshold.get()},
      },
    },
    {
      "fxaa",
      toml::table{
        {"enabled", (bool)RendererCVar::cvar_fxaa_enable.get()},
      },
    },
  };

  std::ofstream file(path);
  file << root;

  return true;
}

bool RendererConfig::load_config(const std::filesystem::path& path) {
  ZoneScoped;
  auto content = File::to_string(path);
  if (content.empty())
    return false;

  toml::table toml = toml::parse(content);

  if (const auto display_config = toml["display"]) {
    if (auto v = display_config["vsync"].as_boolean())
      RendererCVar::cvar_vsync.set(v->get());
  }

  if (const auto debug_config = toml["debug"]) {
    if (auto v = debug_config["debug_renderer"].as_boolean())
      RendererCVar::cvar_enable_debug_renderer.set(v->get());
    if (auto v = debug_config["bounding_boxes"].as_boolean())
      RendererCVar::cvar_draw_bounding_boxes.set(v->get());
    if (auto v = debug_config["physics_debug_renderer"].as_boolean())
      RendererCVar::cvar_enable_physics_debug_renderer.set(v->get());
  }

  if (const auto color_config = toml["color"]) {
    if (auto v = color_config["tonemapper"].as_integer())
      RendererCVar::cvar_tonemapper.set(v->get());
    if (auto v = color_config["exposure"].as_floating_point())
      RendererCVar::cvar_exposure.set(v->get());
    if (auto v = color_config["gamma"].as_floating_point())
      RendererCVar::cvar_gamma.set(v->get());
  }

  if (const auto gtao_config = toml["gtao"]) {
    if (auto v = gtao_config["enabled"].as_boolean())
      RendererCVar::cvar_vbgtao_enable.set(v->get());
    if (auto v = gtao_config["radius"].as_floating_point())
      RendererCVar::cvar_vbgtao_radius.set(v->get());
    if (auto v = gtao_config["quality_level"].as_integer())
      RendererCVar::cvar_vbgtao_quality_level.set(v->get());
  }

  if (const auto bloom_config = toml["bloom"]) {
    if (auto v = bloom_config["enabled"].as_boolean())
      RendererCVar::cvar_bloom_enable.set(v->get());
    if (auto v = bloom_config["threshold"].as_floating_point())
      RendererCVar::cvar_bloom_threshold.set(v->get());
  }

  if (const auto fxaa_config = toml["fxaa"]) {
    if (auto v = fxaa_config["enabled"].as_boolean())
      RendererCVar::cvar_fxaa_enable.set(v->get());
  }

  return true;
}
} // namespace ox
