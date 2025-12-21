#include <Core/AppCommandLineArgs.hpp>
#include <OS/OS.hpp>
#include <ResourceCompiler.hpp>
#include <fmt/base.h>
#include <fmt/chrono.h>

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
  print_command("meta \"path\"", "Add a meta file to compile.");
  print_command("output \"path\"", "Define a path of compiled resources. This is optional.");
  print_command("cache \"path\"", "Add a cache file to avoid recompiling unmodified resources.");
  fmt::println(
    "\n Note: You can still run rcli without specifying any output or cache path to validate integrity of your "
    "resources. Just make sure you don't have --silent flag set."
  );
}

i32 main(i32 argc, c8** argv) {
  auto args = AppCommandLineArgs(argc, argv);
  if (argc <= 1) {
    print_help();
  }

  if (args.contains("--help")) {
    print_help();
    return 0;
  }

  auto silent = args.contains("--silent");

  auto session = rc::Session{};
  auto meta_argi = args.get_index("--meta");
  if (meta_argi.has_value()) {
    auto meta_path = args.get(meta_argi.value() + 1);
    if (!meta_path.has_value()) {
      fmt::println("Specify a meta path.");
      return 1;
    }

    if (!silent) {
      fmt::println("Using meta file \"{}\"...", meta_path.value());
    }

    session = rc::Session::create(0);
    session.import_meta(meta_path.value());
  } else {
    fmt::println("Specify `--meta` flag to use this CLI. Example: `rcli --meta shaders.rcm --output shaders.bin`");
    return 1;
  }

  auto cache_path = option<std::filesystem::path>();
  auto cache_argi = args.get_index("--cache");
  if (cache_argi.has_value()) {
    cache_path = args.get(cache_argi.value() + 1);
    if (!cache_path.has_value()) {
      if (!silent) {
        fmt::println("Specify a cache path.");
      }
      return 1;
    }
  }

  if (cache_path.has_value()) {
    session.import_cache(cache_path.value());
  }

  session.compile_requests();

  auto output_argi = args.get_index("--output");
  if (output_argi.has_value()) {
    auto output_path = args.get(output_argi.value() + 1);
    if (!output_path.has_value()) {
      fmt::println("Specify an output path.");
      return 1;
    }

    session.output_to(output_path.value());
  }

  if (cache_path.has_value()) {
    session.save_cache(cache_path.value());
  }

  if (!silent) {
    auto messages = session.get_messages();
    for (const auto& message : messages) {
      fmt::println("{}", message);
    }
  }

  return 0;
}
