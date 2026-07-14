#pragma once

#include <filesystem>
#include <vector>

#include "Core/Handle.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"

#if OX_PLATFORM_WINDOWS
  #ifdef OXRC_EXPORTS
    #if defined(OX_COMPILER_MSVC) || defined(OX_COMPILER_CLANGCL)
      #define OXRC_API __declspec(dllexport)
    #else
      #define OXRC_API __attribute__((dllexport))
    #endif
  #else
    #if defined(OX_COMPILER_MSVC) || defined(OX_COMPILER_CLANGCL)
      #define OXRC_API __declspec(dllimport)
    #else
      #define OXRC_API __attribute__((dllimport))
    #endif
  #endif
#else
  #ifdef OXRC_EXPORTS
    #define OXRC_API __attribute__((visibility("default")))
  #else
    #define OXRC_API
  #endif
#endif

namespace ox::rc {
struct ShaderSessionInfo {
  std::string name = {};
  std::filesystem::path root_directory = {};
  i32 optimization_level = 3;
  std::vector<std::pair<std::string, std::string>> definitions = {};
};

struct ShaderCompileInfo {
  std::filesystem::path path = {};
  std::string module_name = {};
  std::vector<std::string> entry_points = {};
  bool bindless = false;
};

struct ShaderCompileRequest {
  ShaderSessionInfo session_info = {};
  std::vector<ShaderCompileInfo> shaders = {};
};

struct TextureCompileInfo {
  std::filesystem::path path = {};
  std::string name = {}; // defaults to path.stem() when empty
  bool srgb = false;     // only affects generic (non block-compressed) sources
};

struct TextureCompileRequest {
  std::vector<TextureCompileInfo> textures = {};
};

struct OXRC_API Session : Handle<Session> {
  static auto create() -> option<Session>;
  auto destroy() -> void;

  auto add_request(const ShaderCompileRequest& request) -> void;
  auto add_request(const TextureCompileRequest& request) -> void;
  auto compile() -> bool;
  auto write_to_file(const std::filesystem::path& output_path) -> bool;

  auto push_error(std::string msg) -> void;
  auto push_message(std::string msg) -> void;
  auto get_errors() const -> const std::vector<std::string>&;
  auto get_messages() const -> const std::vector<std::string>&;
};

} // namespace ox::rc
