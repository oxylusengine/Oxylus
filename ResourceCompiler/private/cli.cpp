#include <Core/AppCommandLineArgs.hpp>
#include <ResourceCompiler.hpp>
#include <fmt/base.h>

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
  print_command("meta \"path\"", "Add a meta file to compile.");
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

  auto meta_argi = args.get_index("--meta");
  if (meta_argi.has_value()) {
    auto meta_path = args.get(meta_argi.value() + 1);
    if (!meta_path.has_value()) {
      fmt::println("Specify a meta path.");
      return 1;
    }

    fmt::println("Using meta file \"{}\"...", meta_path.value());
    auto session = rc::Session::create();
    session.import_meta(meta_path.value());
    session.compile_requests();

    auto messages = session.get_messages();
    for (const auto& message : messages) {
      fmt::println("{}", message);
    }
  }

  return 0;
}
