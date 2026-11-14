#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "Core/Option.hpp"

namespace ox {
class Editor;

struct CookStatus {
  std::atomic<bool> is_done = false;
  std::atomic<bool> is_ok = false;
  // written before is_done is released
  std::string error = {};
  std::filesystem::path output = {};
};

auto finish_cook(CookStatus& status, bool is_ok, const std::filesystem::path& output) -> void;

// empty when the form is fine, otherwise what is wrong with it
auto validate_new_project_form(std::string_view location, std::string_view name, std::string_view asset_directory)
  -> std::string_view;

// starts the open and returns, it is finished once the loading panel is past the scan, the start scene is open and
// asset_scan_active() is false. A failure shows in the loading panel
auto open_project(Editor& editor, const std::filesystem::path& project_file) -> std::expected<void, std::string>;
// creates `location/name` with its project file, asset folder and an empty scene
auto new_project(
  Editor& editor, const std::filesystem::path& location, std::string_view name, const std::filesystem::path& asset_dir
) -> std::expected<void, std::string>;
// start_scene is relative to the asset folder
auto set_project(Editor& editor, option<std::string_view> name, option<std::string_view> start_scene)
  -> std::expected<void, std::string>;
auto cook_project(Editor& editor, const std::filesystem::path& folder)
  -> std::expected<std::shared_ptr<CookStatus>, std::string>;
} // namespace ox
