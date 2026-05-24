#include <Core/AppCommandLineArgs.hpp>
#include <ResourceCompiler.hpp>
#include <fmt/base.h>
#include <fmt/std.h>

#include "ResourceConfig.hpp"

using namespace ox;

constexpr auto indentation = 22;
template <typename... Args>
constexpr auto print_command(std::string_view command, fmt::format_string<Args...> desc, Args&&... args) -> void {
  fmt::print("  --{:{}}", command, indentation);
  fmt::println(desc, std::forward<Args>(args)...);
}

auto print_help() -> void {
  fmt::println("### Oxylus Resource Compiler CLI ###");
  print_command("help", "Show list of command line arguments.");
  print_command("silent", "Do not output anything to the console.");
  print_command("config \"path\"", "TOML config file with resources to compile.");
  print_command("output \"path\"", "Output path for compiled resources. Overrides config file output.");
}

auto main(i32 argc, c8** argv) -> i32 {
  auto args = AppCommandLineArgs(argc, argv);

  if (argc <= 1 || args.contains("--help")) {
    print_help();
    return 0;
  }

  auto silent = args.contains("--silent");
  auto log = [silent](std::string_view msg) {
    if (!silent) {
      fmt::println("{}", msg);
    }
  };

  auto config_argi = args.get_index("--config");
  if (!config_argi.has_value()) {
    log("Specify `--config` flag to use this CLI. Example: `rcli --config resources.toml --output shaders.bin`");
    return 1;
  }

  auto config_arg = args.get(config_argi.value() + 1);
  if (!config_arg.has_value()) {
    log("Specify a config file path.");
    return 1;
  }

  auto config_path = std::filesystem::path(config_arg->arg_str);
  log(fmt::format("Using config file \"{}\"...", config_path));

  auto config = rc::parse_resource_config(config_path);
  if (!config.has_value()) {
    log(fmt::format("Error: failed to parse '{}'.", config_path));
    return 1;
  }

  auto session = rc::Session::create();
  if (!session.has_value()) {
    log("Error: failed to create compiler session.");
    return 1;
  }

  auto config_dir = std::filesystem::absolute(config_path).parent_path();

  for (const auto& shader_session : config->shader_sessions) {
    auto root = (config_dir / shader_session.root_directory).lexically_normal();

    auto request = rc::ShaderCompileRequest{
      .session_info = {
        .name = shader_session.session_name,
        .root_directory = root,
        .optimization_level = shader_session.optimization_level,
        .definitions = shader_session.definitions,
      },
    };

    for (const auto& prog : shader_session.programs) {
      request.shaders.push_back({
        .path = prog.path,
        .module_name = prog.name,
        .entry_points = prog.entry_points,
      });
    }

    session->add_request(request);
  }

  auto compile_success = session->compile();

  // Print collected errors
  for (const auto& error : session->get_errors()) {
    fmt::println("Error: {}", error);
  }

  // Print collected messages
  for (const auto& msg : session->get_messages()) {
    log(msg);
  }

  if (!compile_success) {
    return 1;
  }

  auto output_argi = args.get_index("--output");
  if (output_argi.has_value()) {
    auto output_arg = args.get(output_argi.value() + 1);
    if (!output_arg.has_value()) {
      log("Specify an output path.");
      return 1;
    }

    auto output_path = std::filesystem::path(output_arg->arg_str);
    if (!session->write_to_file(output_path)) {
      for (const auto& error : session->get_errors()) {
        fmt::println("Error: {}", error);
      }
      return 1;
    }

    usize total = 0;
    for (const auto& s : config->shader_sessions) {
      total += s.programs.size();
    }
    log(fmt::format("Compiled {} program(s) -> {}", total, output_path.filename().string()));
  } else {
    for (const auto& shader_session : config->shader_sessions) {
      if (shader_session.output.empty()) {
        continue;
      }
      if (!session->write_to_file(shader_session.output)) {
        for (const auto& error : session->get_errors()) {
          fmt::println("Error: {}", error);
        }
        return 1;
      }
      log(fmt::format(
        "Compiled {} program(s) -> {}",
        shader_session.programs.size(),
        shader_session.output.filename().string()
      ));
    }
  }

  return 0;
}
