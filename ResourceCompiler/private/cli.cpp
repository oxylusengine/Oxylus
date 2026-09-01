#include <Core/AppCommandLineArgs.hpp>
#include <ResourceCompiler.hpp>
#include <charconv>
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
  print_command("include-dir \"path\"", "Extra shader search path, appended to every session. Repeatable.");
  print_command("threads N", "Number of shader codegen workers. Defaults to the hardware concurrency, capped.");
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

  auto session_info = rc::SessionCreateInfo{};
  auto threads_argi = args.get_index("--threads");
  if (threads_argi.has_value()) {
    auto threads_arg = args.get(threads_argi.value() + 1);
    if (!threads_arg.has_value()) {
      log("Specify a thread count after `--threads`.");
      return 1;
    }

    auto thread_count = 0;
    const auto* begin = threads_arg->arg_str.data();
    const auto* end = begin + threads_arg->arg_str.size();
    if (std::from_chars(begin, end, thread_count).ec != std::errc{} || thread_count < 1) {
      log(fmt::format("Error: `--threads` expects a positive integer, got '{}'.", threads_arg->arg_str));
      return 1;
    }

    session_info.thread_count = static_cast<u32>(thread_count);
  }

  auto session = rc::Session::create(session_info);
  if (!session.has_value()) {
    log("Error: failed to create compiler session.");
    return 1;
  }

  auto config_dir = std::filesystem::absolute(config_path).parent_path();

  // Repeatable. The `compile_shaders` xmake rule uses this to hand downstream projects the engine
  // shader tree without baking an absolute path into their config.
  auto cli_include_dirs = std::vector<std::filesystem::path>{};
  for (const auto& arg : args.args) {
    if (arg.arg_str != "--include-dir") {
      continue;
    }

    auto include_arg = args.get(arg.arg_index + 1);
    if (!include_arg.has_value()) {
      log("Specify a path after `--include-dir`.");
      return 1;
    }

    cli_include_dirs.emplace_back(std::filesystem::absolute(include_arg->arg_str).lexically_normal());
  }

  for (const auto& shader_session : config->shader_sessions) {
    auto root = (config_dir / shader_session.root_directory).lexically_normal();

    // Config-declared paths win over the ones the build system injected.
    auto include_dirs = std::vector<std::filesystem::path>{};
    include_dirs.reserve(shader_session.include_directories.size() + cli_include_dirs.size());
    for (const auto& include_dir : shader_session.include_directories) {
      include_dirs.emplace_back((config_dir / include_dir).lexically_normal());
    }
    include_dirs.insert(include_dirs.end(), cli_include_dirs.begin(), cli_include_dirs.end());

    auto request = rc::ShaderCompileRequest{
      .session_info = {
        .name = shader_session.session_name,
        .root_directory = root,
        .include_directories = std::move(include_dirs),
        .optimization_level = shader_session.optimization_level,
        .definitions = shader_session.definitions,
      },
    };

    for (const auto& prog : shader_session.programs) {
      request.shaders.push_back({
        .path = prog.path,
        .module_name = prog.name,
        .entry_points = prog.entry_points,
        .required_features = prog.required_features,
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
      log(
        fmt::format(
          "Compiled {} program(s) -> {}",
          shader_session.programs.size(),
          shader_session.output.filename().string()
        )
      );
    }
  }

  return 0;
}
