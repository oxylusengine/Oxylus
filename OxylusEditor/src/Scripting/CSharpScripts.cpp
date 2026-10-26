#include "Scripting/CSharpScripts.hpp"

#include <FileWatch.hpp>
#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <array>
#include <fmt/format.h>

#include "Core/App.hpp"
#include "Project/Project.hpp"
#include "Scripting/DotNetScripting.hpp"
#include "Utils/Log.hpp"

namespace ox {
// typing in an IDE saves several files in a burst, one build covers them all
constexpr auto BUILD_DEBOUNCE = std::chrono::milliseconds(300);

static auto is_script_source(std::string_view relative_path) -> bool {
  constexpr auto EXTENSIONS = std::array<std::string_view, 3>{".cs", ".csproj", ".props"};
  const auto is_source = std::ranges::any_of(EXTENSIONS, [relative_path](std::string_view extension) {
    return relative_path.ends_with(extension);
  });
  if (!is_source) {
    return false;
  }

  // what the build itself writes
  const auto path = std::filesystem::path(relative_path);
  return std::ranges::none_of(path, [](const std::filesystem::path& part) { return part == "obj" || part == "bin"; });
}

static auto log_diagnostic(const DotNetDiagnostic& diagnostic) -> void {
  const auto is_error = diagnostic.severity == DotNetDiagnosticSeverity::Error;
  const auto kind = is_error ? "error" : "warning";
  if (diagnostic.file.empty()) {
    if (is_error) {
      OX_LOG_ERROR("C# {} {}: {}", kind, diagnostic.code, diagnostic.message);
    } else {
      OX_LOG_WARN("C# {} {}: {}", kind, diagnostic.code, diagnostic.message);
    }

    return;
  }

  // the form IDEs and terminals turn into a link to the line
  if (is_error) {
    OX_LOG_ERROR(
      "{}({},{}): {} {}: {}",
      diagnostic.file,
      diagnostic.line,
      diagnostic.column,
      kind,
      diagnostic.code,
      diagnostic.message
    );
  } else {
    OX_LOG_WARN(
      "{}({},{}): {} {}: {}",
      diagnostic.file,
      diagnostic.line,
      diagnostic.column,
      kind,
      diagnostic.code,
      diagnostic.message
    );
  }
}

CSharpScripts::CSharpScripts() = default;

CSharpScripts::~CSharpScripts() = default;

auto CSharpScripts::create_project(Project& project) -> std::expected<void, std::string> {
  ZoneScoped;

  auto& config = project.get_config();
  if (!config.scripts_project.empty()) {
    return {};
  }

  const auto& project_dir = project.get_project_directory();
  auto csproj = create_scripts_project(project_dir / "Scripts", config.name);
  if (!csproj) {
    return std::unexpected(std::move(csproj.error()));
  }

  config.scripts_project = std::filesystem::relative(*csproj, project_dir);
  if (!project.save(project.get_project_file_path())) {
    return std::unexpected(fmt::format("Couldn't save {}", project.get_project_file_path().string()));
  }

  return {};
}

auto CSharpScripts::open(this CSharpScripts& self, Project& project) -> void {
  ZoneScoped;

  self.close();

  const auto& project_config = project.get_config();
  if (project_config.scripts_project.empty() || !App::has_mod<DotNetScripting>()) {
    return;
  }

  const auto& project_dir = project.get_project_directory();
  self.csproj_ = project_dir / project_config.scripts_project;
  self.output_dir_ = project_dir / ".oxeditor" / "managed";
  self.sdk_dir_ = std::filesystem::path(SDL_GetBasePath()) / "managed";
  const auto asset_dir = project_dir / project_config.asset_directory;
  if (!std::filesystem::exists(self.csproj_)) {
    OX_LOG_ERROR("The project's C# project {} doesn't exist", self.csproj_);
    self.csproj_.clear();
    return;
  }

  if (auto written = write_scripts_props(self.csproj_.parent_path(), self.sdk_dir_, asset_dir); !written) {
    OX_LOG_ERROR("{}", written.error());
  }

  if (auto written = write_scripts_solution(project_dir, self.csproj_); !written) {
    OX_LOG_ERROR("{}", written.error());
  }

  const auto on_change = [&self](const std::string& path, filewatch::Event) {
    if (is_script_source(path)) {
      self.is_dirty_.store(true, std::memory_order_relaxed);
    }
  };
  self.scripts_watch_ = std::make_unique<filewatch::FileWatch<std::string>>(
    self.csproj_.parent_path().string(),
    on_change
  );
  if (std::filesystem::exists(asset_dir)) {
    self.assets_watch_ = std::make_unique<filewatch::FileWatch<std::string>>(asset_dir.string(), on_change);
  }

  // incremental, so a project whose scripts are already built is ready in a moment
  self.start_build();
}

auto CSharpScripts::close(this CSharpScripts& self) -> void {
  ZoneScoped;

  self.scripts_watch_.reset();
  self.assets_watch_.reset();
  if (self.build_.valid()) {
    self.build_.wait();
    self.build_ = {};
  }

  if (!self.csproj_.empty() && App::has_mod<DotNetScripting>()) {
    App::mod<DotNetScripting>().unload();
  }

  self.csproj_.clear();
  self.is_dirty_ = false;
  self.dirty_since_ = nullopt;
  self.is_build_failed_ = false;
  self.is_reload_pending_ = false;
}

auto CSharpScripts::is_open(this const CSharpScripts& self) -> bool { return !self.csproj_.empty(); }

auto CSharpScripts::update(this CSharpScripts& self, bool is_playing) -> void {
  ZoneScoped;

  if (!self.is_open()) {
    return;
  }

  if (self.build_.valid() && self.build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    self.finish_build();
  }

  // a scene that is playing keeps the code it started with
  if (self.is_reload_pending_ && !is_playing) {
    self.reload();
  }

  if (!self.is_dirty_.load(std::memory_order_relaxed) || self.build_.valid()) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (!self.dirty_since_.has_value()) {
    self.dirty_since_ = now;
    return;
  }

  if (now - *self.dirty_since_ >= BUILD_DEBOUNCE) {
    self.start_build();
  }
}

auto CSharpScripts::prepare_for_play(this CSharpScripts& self) -> bool {
  ZoneScoped;

  if (!self.is_open()) {
    return true;
  }

  if (self.is_dirty_.load(std::memory_order_relaxed) && !self.build_.valid()) {
    self.start_build();
  }

  if (self.build_.valid()) {
    self.build_.wait();
    self.finish_build();
  }

  if (self.is_build_failed_) {
    OX_LOG_ERROR("Fix the C# errors before playing");
    return false;
  }

  if (self.is_reload_pending_) {
    self.reload();
  }

  return true;
}

auto CSharpScripts::request_build(this CSharpScripts& self) -> void {
  self.is_dirty_ = true;
  self.dirty_since_ = std::chrono::steady_clock::now() - BUILD_DEBOUNCE;
}

auto CSharpScripts::is_building(this const CSharpScripts& self) -> bool { return self.build_.valid(); }

auto CSharpScripts::start_build(this CSharpScripts& self) -> void {
  ZoneScoped;

  // cleared first, a change landing while dotnet runs builds again after
  self.is_dirty_ = false;
  self.dirty_since_ = nullopt;
  OX_LOG_INFO("Building C# scripts {}", self.csproj_.filename());
  self.build_ = std::async(std::launch::async, [csproj = self.csproj_, output_dir = self.output_dir_] {
    return build_scripts_project(csproj, output_dir);
  });
}

auto CSharpScripts::finish_build(this CSharpScripts& self) -> void {
  ZoneScoped;

  const auto result = self.build_.get();
  // a machine without the SDK can still edit and play the scene, just without its C#
  if (result.is_dotnet_missing) {
    OX_LOG_WARN("C# scripts aren't built, dotnet couldn't be started: {}", result.diagnostics.front().message);
    self.is_build_failed_ = false;
    return;
  }

  for (const auto& diagnostic : result.diagnostics) {
    log_diagnostic(diagnostic);
  }

  self.is_build_failed_ = !result.is_success;
  if (self.is_build_failed_) {
    const auto error_count = std::ranges::count_if(result.diagnostics, [](const DotNetDiagnostic& diagnostic) {
      return diagnostic.severity == DotNetDiagnosticSeverity::Error;
    });
    OX_LOG_ERROR("C# scripts failed to build with {} errors, the editor keeps the last good build", error_count);
    return;
  }

  self.is_reload_pending_ = true;
}

auto CSharpScripts::reload(this CSharpScripts& self) -> void {
  ZoneScoped;

  self.is_reload_pending_ = false;
  const auto info = DotNetLoadInfo{
    .kind = DotNetHostKind::HostFxr,
    .path = scripts_assembly_path(self.csproj_, self.output_dir_),
    .scripting_directory = self.sdk_dir_,
  };
  if (auto loaded = App::mod<DotNetScripting>().load(info); !loaded) {
    OX_LOG_ERROR("{}", loaded.error());
    return;
  }

  OX_LOG_INFO("Loaded C# scripts {}", info.path.filename());
}
} // namespace ox
