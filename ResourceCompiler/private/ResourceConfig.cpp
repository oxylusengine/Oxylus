#include "ResourceConfig.hpp"

#include <fmt/base.h>
#include <fmt/std.h>
#include <toml++/toml.hpp>

#include "OS/File.hpp"

namespace ox::rc {
auto parse_resource_config(const std::filesystem::path& config_path) -> option<ResourceConfig> {
  auto content = File::to_string(config_path);
  if (content.empty()) {
    return nullopt;
  }

  toml::table root;
  try {
    root = toml::parse(content, config_path.string());
  } catch (const toml::parse_error& err) {
    fmt::println("Error parsing '{}': {}", config_path.string(), err.what());
    return nullopt;
  }

  auto config = ResourceConfig{};

  if (auto v = root["version"].as_integer()) {
    config.version = static_cast<i32>(v->get());
  }

  // [[shader_sessions]]
  auto* sessions = root["shader_sessions"].as_array();
  if (!sessions) {
    fmt::println("Error: missing [[shader_sessions]] in '{}'.", config_path.string());
    return nullopt;
  }

  for (const auto& session_elem : *sessions) {
    auto* tbl = session_elem.as_table();
    if (!tbl) {
      continue;
    }

    auto session = ShaderSessionConfig{};

    if (auto node = (*tbl)["root_directory"].as_string()) {
      session.root_directory = node->get();
    } else {
      fmt::println("Error: shader session missing 'root_directory'.");
      return nullopt;
    }

    if (auto node = (*tbl)["session_name"].as_string()) {
      session.session_name = node->get();
    }

    if (auto node = (*tbl)["debug_symbols"].as_boolean()) {
      session.debug_symbols = node->get();
    }

    if (auto node = (*tbl)["output"].as_string()) {
      session.output = node->get();
    }

    if (auto opt_str = (*tbl)["optimization"].as_string()) {
      auto val = std::string_view(opt_str->get());
      if (val == "none") {
        session.optimization_level = 0;
      } else if (val == "default") {
        session.optimization_level = 1;
      } else if (val == "full" || val == "max") {
        session.optimization_level = 3;
      }
    } else if (auto opt_int = (*tbl)["optimization"].as_integer()) {
      session.optimization_level = static_cast<i32>(opt_int->get());
    }

    // [[shader_sessions.definitions]]
    if (auto* defs = (*tbl)["definitions"].as_array()) {
      for (const auto& def_elem : *defs) {
        auto* def_tbl = def_elem.as_table();
        if (!def_tbl) {
          continue;
        }
        auto name = std::string{};
        if (auto n = (*def_tbl)["name"].as_string()) {
          name = n->get();
        }
        if (name.empty()) {
          continue;
        }
        if (auto str_val = (*def_tbl)["value"].as_string()) {
          session.definitions.emplace_back(name, str_val->get());
        } else if (auto int_val = (*def_tbl)["value"].as_integer()) {
          session.definitions.emplace_back(name, std::to_string(int_val->get()));
        } else if (auto bool_val = (*def_tbl)["value"].as_boolean()) {
          session.definitions.emplace_back(name, bool_val->get() ? "1" : "0");
        } else {
          session.definitions.emplace_back(name, "1");
        }
      }
    }

    // [[shader_sessions.programs]]
    auto* programs = (*tbl)["programs"].as_array();
    if (!programs || programs->empty()) {
      fmt::println("Error: shader session '{}' has no [[programs]].", session.session_name);
      return nullopt;
    }

    for (const auto& prog_elem : *programs) {
      auto* prog_tbl = prog_elem.as_table();
      if (!prog_tbl) {
        continue;
      }

      auto prog = ShaderProgramConfig{};

      if (auto node = (*prog_tbl)["name"].as_string()) {
        prog.name = node->get();
      }
      if (auto node = (*prog_tbl)["path"].as_string()) {
        prog.path = node->get();
      }
      if (prog.name.empty() && !prog.path.empty()) {
        prog.name = prog.path.stem().string();
      }

      if (auto* eps = (*prog_tbl)["entry_points"].as_array()) {
        for (const auto& ep : *eps) {
          if (auto ep_str = ep.as_string()) {
            prog.entry_points.push_back(ep_str->get());
          }
        }
      }

      session.programs.push_back(std::move(prog));
    }

    config.shader_sessions.push_back(std::move(session));
  }

  // [[models]] (optional, for future use)
  if (auto* models = root["models"].as_array()) {
    for (const auto& model_elem : *models) {
      auto* tbl = model_elem.as_table();
      if (!tbl) {
        continue;
      }
      auto model = ModelConfig{};
      if (auto node = (*tbl)["path"].as_string()) {
        model.path = node->get();
      }
      if (auto node = (*tbl)["is_foliage"].as_boolean()) {
        model.is_foliage = node->get();
      }
      config.models.push_back(std::move(model));
    }
  }

  return config;
}

} // namespace ox::rc
