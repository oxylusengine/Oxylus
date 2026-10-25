#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <string>

#include "Core/Option.hpp"
#include "Scripting/DotNetProject.hpp"

namespace filewatch {
template <class StringType>
class FileWatch;
}

namespace ox {
class Project;

// keeps the editor on the latest build of a project's C# scripts. A changed .cs file rebuilds in the background, and
// a good build reloads right away unless a scene is playing, in which case it waits for the stop
class CSharpScripts {
public:
  CSharpScripts();
  ~CSharpScripts();

  CSharpScripts(const CSharpScripts&) = delete;
  CSharpScripts& operator=(const CSharpScripts&) = delete;

  // gives a project without one its Scripts/<Name>.csproj and records it in the project file
  static auto create_project(Project& project) -> std::expected<void, std::string>;

  auto open(this CSharpScripts& self, Project& project) -> void;
  auto close(this CSharpScripts& self) -> void;
  auto is_open(this const CSharpScripts& self) -> bool;

  // main thread, once a frame
  auto update(this CSharpScripts& self, bool is_playing) -> void;
  // finishes or runs the build Play needs and reloads it, false when the scripts don't compile
  auto prepare_for_play(this CSharpScripts& self) -> bool;
  auto request_build(this CSharpScripts& self) -> void;
  auto is_building(this const CSharpScripts& self) -> bool;

private:
  std::filesystem::path csproj_ = {};
  std::filesystem::path output_dir_ = {};
  std::filesystem::path sdk_dir_ = {};

  std::unique_ptr<filewatch::FileWatch<std::string>> scripts_watch_;
  std::unique_ptr<filewatch::FileWatch<std::string>> assets_watch_;
  // written by the watcher threads
  std::atomic<bool> is_dirty_ = false;
  option<std::chrono::steady_clock::time_point> dirty_since_ = nullopt;

  std::future<DotNetBuildResult> build_ = {};
  bool is_build_failed_ = false;
  bool is_reload_pending_ = false;

  auto start_build(this CSharpScripts& self) -> void;
  auto finish_build(this CSharpScripts& self) -> void;
  auto reload(this CSharpScripts& self) -> void;
};
} // namespace ox
