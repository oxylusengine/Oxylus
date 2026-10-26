#include "Scripting/DotNetProject.hpp"

#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <fstream>
#include <regex>
#include <system_error>

#include "Core/UUID.hpp"
#include "DotNetHosts.hpp"

namespace ox {
static auto xml_escape(std::string_view text) -> std::string {
  auto escaped = std::string();
  escaped.reserve(text.size());
  for (const auto c : text) {
    switch (c) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      default : escaped += c; break;
    }
  }

  return escaped;
}

// forward slashes and a trailing one, how MSBuild properties naming a directory are written
static auto msbuild_dir(const std::filesystem::path& dir) -> std::string {
  auto text = to_utf8(std::filesystem::absolute(dir).lexically_normal());
  std::ranges::replace(text, '\\', '/');
  if (!text.ends_with('/')) {
    text += '/';
  }

  return xml_escape(text);
}

static auto write_file(const std::filesystem::path& path, std::string_view content)
  -> std::expected<void, std::string> {
  auto error = std::error_code();
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return std::unexpected(fmt::format("Couldn't create {}: {}", to_utf8(path.parent_path()), error.message()));
  }

  auto file = std::ofstream(path, std::ios::binary);
  file << content;
  if (!file) {
    return std::unexpected(fmt::format("Couldn't write {}", to_utf8(path)));
  }

  return {};
}

// MSBuild names a tool rather than a file for errors that have no location, "CSC" or "MSBUILD"
static auto is_tool_origin(std::string_view origin) -> bool {
  return !origin.empty() && std::ranges::all_of(origin, [](c8 c) { return std::isupper(static_cast<u8>(c)) != 0; });
}

auto to_csharp_identifier(std::string_view name) -> std::string {
  auto identifier = std::string();
  auto is_word_start = true;
  for (const auto c : name) {
    if (!std::isalnum(static_cast<u8>(c)) && c != '_') {
      is_word_start = true;
      continue;
    }

    identifier += is_word_start ? static_cast<c8>(std::toupper(static_cast<u8>(c))) : c;
    is_word_start = false;
  }

  if (identifier.empty()) {
    return "Game";
  }

  if (std::isdigit(static_cast<u8>(identifier.front()))) {
    identifier.insert(identifier.begin(), '_');
  }

  return identifier;
}

auto create_scripts_project(const std::filesystem::path& scripts_dir, std::string_view project_name)
  -> std::expected<std::filesystem::path, std::string> {
  ZoneScoped;

  const auto csproj = scripts_dir / fmt::format("{}.csproj", to_csharp_identifier(project_name));
  if (std::filesystem::exists(csproj)) {
    return csproj;
  }

  constexpr auto CONTENT = R"(<Project Sdk="Microsoft.NET.Sdk">
  <!-- the editor rewrites Oxylus.props whenever the project opens, put settings of your own here instead -->
  <Import Project="Oxylus.props" />

  <PropertyGroup>
    <Nullable>enable</Nullable>
  </PropertyGroup>
</Project>
)";

  if (auto written = write_file(csproj, CONTENT); !written) {
    return std::unexpected(std::move(written.error()));
  }

  return csproj;
}

auto write_scripts_props(
  const std::filesystem::path& scripts_dir, const std::filesystem::path& sdk_dir, const std::filesystem::path& asset_dir
) -> std::expected<void, std::string> {
  ZoneScoped;

  const auto content = fmt::format(
    R"(<Project>
  <!-- written by the Oxylus editor every time the project opens, edit the csproj instead. A game's build sets both
       properties itself and imports the Oxylus.Sdk.props its engine package installed -->
  <PropertyGroup>
    <OxylusSdkDir>{0}</OxylusSdkDir>
    <OxylusAssetDir>{1}</OxylusAssetDir>
  </PropertyGroup>

  <Import Project="$(OxylusSdkDir)Oxylus.Sdk.props" />
</Project>
)",
    msbuild_dir(sdk_dir),
    msbuild_dir(asset_dir)
  );

  return write_file(scripts_dir / "Oxylus.props", content);
}

auto write_scripts_solution(const std::filesystem::path& project_dir, const std::filesystem::path& csproj)
  -> std::expected<void, std::string> {
  ZoneScoped;

  auto error = std::error_code();
  for (const auto& entry : std::filesystem::directory_iterator(project_dir, error)) {
    const auto extension = entry.path().extension();
    if (extension == ".sln" || extension == ".slnx") {
      return {};
    }
  }

  if (error) {
    return std::unexpected(fmt::format("Couldn't read {}: {}", to_utf8(project_dir), error.message()));
  }

  auto relative_csproj = to_utf8(std::filesystem::relative(csproj, project_dir).lexically_normal());
  std::ranges::replace(relative_csproj, '/', '\\');
  auto guid = UUID::generate_random().str();
  std::ranges::transform(guid, guid.begin(), [](c8 c) { return static_cast<c8>(std::toupper(static_cast<u8>(c))); });

  // scripts live in the asset directory, outside the csproj's folder, so an LSP starting from one walks up to this
  const auto content = fmt::format(
    "Microsoft Visual Studio Solution File, Format Version 12.00\r\n"
    "# Visual Studio Version 17\r\n"
    "VisualStudioVersion = 17.0.31903.59\r\n"
    "MinimumVisualStudioVersion = 10.0.40219.1\r\n"
    "Project(\"{{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}}\") = \"{0}\", \"{1}\", \"{{{2}}}\"\r\n"
    "EndProject\r\n"
    "Global\r\n"
    "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n"
    "\t\tDebug|Any CPU = Debug|Any CPU\r\n"
    "\t\tRelease|Any CPU = Release|Any CPU\r\n"
    "\tEndGlobalSection\r\n"
    "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n"
    "\t\t{{{2}}}.Debug|Any CPU.ActiveCfg = Debug|Any CPU\r\n"
    "\t\t{{{2}}}.Debug|Any CPU.Build.0 = Debug|Any CPU\r\n"
    "\t\t{{{2}}}.Release|Any CPU.ActiveCfg = Release|Any CPU\r\n"
    "\t\t{{{2}}}.Release|Any CPU.Build.0 = Release|Any CPU\r\n"
    "\tEndGlobalSection\r\n"
    "EndGlobal\r\n",
    to_utf8(csproj.stem()),
    relative_csproj,
    guid
  );

  return write_file(project_dir / csproj.stem().concat(".sln"), content);
}

auto scripts_assembly_path(const std::filesystem::path& csproj, const std::filesystem::path& output_dir)
  -> std::filesystem::path {
  // the default AssemblyName is the project file's name
  return output_dir / csproj.stem().concat(".dll");
}

auto csharp_behaviour_template(std::string_view namespace_name, std::string_view class_name) -> std::string {
  return fmt::format(
    R"(using Oxylus;

namespace {0};

public sealed class {1} : EntityBehaviour
{{
    public override void OnStart()
    {{
    }}

    public override void OnUpdate(float deltaTime)
    {{
    }}
}}
)",
    namespace_name,
    class_name
  );
}

auto parse_msbuild_output(std::string_view output) -> std::vector<DotNetDiagnostic> {
  ZoneScoped;

  // origin(line,column): [subcategory] error|warning code: text [project]
  static const auto pattern = std::regex(
    R"(^\s*(.*?)(?:\((\d+)(?:,(\d+))?(?:,\d+,\d+)?\))?\s*:\s*(?:\w+\s+)*?(error|warning)\s*([A-Za-z]+\d+)?\s*:\s*(.*?)(?:\s+\[[^\]]*\])?\s*$)"
  );

  auto diagnostics = std::vector<DotNetDiagnostic>();
  auto rest = output;
  while (!rest.empty()) {
    const auto end = rest.find('\n');
    auto line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }

    auto match = std::match_results<std::string_view::const_iterator>();
    if (!std::regex_match(line.begin(), line.end(), match, pattern)) {
      continue;
    }

    const auto origin = std::string_view(match[1].first, match[1].second);
    auto diagnostic = DotNetDiagnostic{
      .severity = match[4] == "error" ? DotNetDiagnosticSeverity::Error : DotNetDiagnosticSeverity::Warning,
      .file = is_tool_origin(origin) ? std::string() : std::string(origin),
      .line = match[2].matched ? static_cast<u32>(std::stoul(match[2].str())) : 0,
      .column = match[3].matched ? static_cast<u32>(std::stoul(match[3].str())) : 0,
      .code = match[5].str(),
      .message = match[6].str(),
    };

    // MSBuild repeats a diagnostic once per target that reports it
    const auto is_repeat = std::ranges::any_of(diagnostics, [&diagnostic](const DotNetDiagnostic& seen) {
      return seen.severity == diagnostic.severity && seen.file == diagnostic.file && seen.line == diagnostic.line &&
             seen.column == diagnostic.column && seen.code == diagnostic.code && seen.message == diagnostic.message;
    });
    if (!is_repeat) {
      diagnostics.push_back(std::move(diagnostic));
    }
  }

  return diagnostics;
}

auto build_scripts_project(const std::filesystem::path& csproj, const std::filesystem::path& output_dir)
  -> DotNetBuildResult {
  ZoneScoped;

  const auto project = to_utf8(csproj);
  const auto output = to_utf8(output_dir);
  // quiet with no summary leaves one line per diagnostic, and no terminal logger since the output is piped
  const c8* args[] = {
    "dotnet",
    "build",
    project.c_str(),
    "-c",
    "Debug",
    "-o",
    output.c_str(),
    "-nologo",
    "-v:quiet",
    "-tl:off",
    "-clp:NoSummary",
    nullptr,
  };

  auto* environment = SDL_CreateEnvironment(true);
  SDL_SetEnvironmentVariable(environment, "DOTNET_NOLOGO", "1", true);
  SDL_SetEnvironmentVariable(environment, "DOTNET_CLI_TELEMETRY_OPTOUT", "1", true);

  const auto properties = SDL_CreateProperties();
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, static_cast<void*>(args));
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);

  auto* process = SDL_CreateProcessWithProperties(properties);
  SDL_DestroyProperties(properties);
  SDL_DestroyEnvironment(environment);
  if (!process) {
    return DotNetBuildResult{
      .is_dotnet_missing = true,
      .diagnostics = {
        {.message = fmt::format("Couldn't run dotnet, is the .NET 10 SDK installed? {}", SDL_GetError())}
      },
    };
  }

  auto size = usize{0};
  auto exit_code = 0;
  auto* data = static_cast<c8*>(SDL_ReadProcess(process, &size, &exit_code));
  auto result = DotNetBuildResult{.output = data ? std::string(data, size) : std::string()};
  SDL_free(data);
  SDL_DestroyProcess(process);

  result.diagnostics = parse_msbuild_output(result.output);
  result.is_success = exit_code == 0;
  if (!result.is_success && std::ranges::none_of(result.diagnostics, [](const DotNetDiagnostic& diagnostic) {
        return diagnostic.severity == DotNetDiagnosticSeverity::Error;
      })) {
    result.diagnostics.push_back({.message = fmt::format("dotnet build exited with {}: {}", exit_code, result.output)});
  }

  return result;
}
} // namespace ox
