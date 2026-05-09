#pragma once

#include <filesystem>
#include <source_location>
#include <vector>

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
enum class ShaderOptimization : i32 {
  None = 0,
  Default,
  High,
  Maximal,
};

struct ShaderSessionInfo {
  std::string name = {};
  std::filesystem::path root_directory = {};
  ShaderOptimization optimization = ShaderOptimization::Default;
  bool debug_symbols = false;
  std::vector<std::pair<std::string, std::string>> definitions = {};
};

struct ShaderInfo {
  std::filesystem::path path = {};
  std::string module_name = {};
  std::vector<std::string> entry_points = {};
};

struct ShaderCompileRequest {
  ShaderSessionInfo session_info = {};
  std::vector<ShaderInfo> shader_infos = {};
};
} // namespace ox::rc
