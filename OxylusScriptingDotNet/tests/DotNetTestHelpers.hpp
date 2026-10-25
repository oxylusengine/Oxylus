#pragma once

#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Scripting/DotNetScripting.hpp"

struct CapturedLog {
  ox::OxLogLevel level = ox::OxLogLevel::Info;
  std::string message = {};
};

inline auto captured_logs = std::vector<CapturedLog>{};

inline auto capture_log(ox::OxLogLevel level, const c8* message, usize length) -> void {
  captured_logs.push_back({.level = level, .message = std::string(message, length)});
}

inline auto capturing_native_api() -> ox::OxNativeApi {
  auto native_api = ox::make_native_api();
  native_api.log = capture_log;
  return native_api;
}

inline auto count_logs(ox::OxLogLevel level, std::string_view message) -> usize {
  return static_cast<usize>(std::ranges::count_if(captured_logs, [&](const CapturedLog& log) {
    return log.level == level && log.message == message;
  }));
}

inline auto count_logs_containing(ox::OxLogLevel level, std::string_view text) -> usize {
  return static_cast<usize>(std::ranges::count_if(captured_logs, [&](const CapturedLog& log) {
    return log.level == level && log.message.find(text) != std::string::npos;
  }));
}

// for a failed expectation's message, what the scripts logged
inline auto logs_text() -> std::string {
  auto text = std::string("captured logs:\n");
  for (const auto& log : captured_logs) {
    text += "  ";
    text += log.message;
    text += '\n';
  }

  return text;
}

inline auto count_logs(ox::OxLogLevel level) -> usize {
  return static_cast<usize>(std::ranges::count_if(captured_logs, [&](const CapturedLog& log) {
    return log.level == level;
  }));
}

// the build puts the C# outputs next to the test binary, wherever the test runner starts it from
inline auto binary_dir() -> std::filesystem::path { return std::filesystem::path(SDL_GetBasePath()); }

#if defined(OX_PLATFORM_WINDOWS)
constexpr auto SMOKE_GAME_LIBRARY = "SmokeGame.dll";
#elif defined(OX_PLATFORM_MACOSX)
constexpr auto SMOKE_GAME_LIBRARY = "SmokeGame.dylib";
#else
constexpr auto SMOKE_GAME_LIBRARY = "SmokeGame.so";
#endif

inline auto hostfxr_smoke_game() -> ox::DotNetLoadInfo {
  return {.kind = ox::DotNetHostKind::HostFxr, .path = binary_dir() / "managed" / "SmokeGame.dll"};
}

inline auto aot_smoke_game() -> ox::DotNetLoadInfo {
  return {.kind = ox::DotNetHostKind::Aot, .path = binary_dir() / "aot" / SMOKE_GAME_LIBRARY};
}
