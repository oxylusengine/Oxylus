#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Types.hpp"

namespace ox {
enum class DotNetDiagnosticSeverity : u8 {
  Warning,
  Error,
};

struct DotNetDiagnostic {
  DotNetDiagnosticSeverity severity = DotNetDiagnosticSeverity::Error;
  // empty for what MSBuild reports without a location, like a missing SDK
  std::string file = {};
  u32 line = 0;
  u32 column = 0;
  std::string code = {};
  std::string message = {};
};

struct DotNetBuildResult {
  bool is_success = false;
  // dotnet itself couldn't be started, which is no fault of the scripts
  bool is_dotnet_missing = false;
  std::vector<DotNetDiagnostic> diagnostics = {};
  // everything dotnet printed, for a failure that reported nothing parseable
  std::string output = {};
};

// "My Game 2" -> "MyGame2", usable as an assembly name and a namespace
auto to_csharp_identifier(std::string_view name) -> std::string;

// writes <scripts_dir>/<identifier>.csproj, which the developer owns from then on, unless it exists. Returns its path
auto create_scripts_project(const std::filesystem::path& scripts_dir, std::string_view project_name)
  -> std::expected<std::filesystem::path, std::string>;

// rewrites <scripts_dir>/Oxylus.props, which the csproj imports: `sdk_dir` and `asset_dir`, and the Oxylus.Sdk.props in
// `sdk_dir` that holds everything else. The editor writes it every time the project opens, so it always matches the
// editor in use
auto write_scripts_props(
  const std::filesystem::path& scripts_dir, const std::filesystem::path& sdk_dir, const std::filesystem::path& asset_dir
) -> std::expected<void, std::string>;

// the dll a build of `csproj` into `output_dir` produces
auto scripts_assembly_path(const std::filesystem::path& csproj, const std::filesystem::path& output_dir)
  -> std::filesystem::path;

auto csharp_behaviour_template(std::string_view namespace_name, std::string_view class_name) -> std::string;

auto parse_msbuild_output(std::string_view output) -> std::vector<DotNetDiagnostic>;

// runs `dotnet build` and blocks until it exits, so call it off the main thread
auto build_scripts_project(const std::filesystem::path& csproj, const std::filesystem::path& output_dir)
  -> DotNetBuildResult;
} // namespace ox
