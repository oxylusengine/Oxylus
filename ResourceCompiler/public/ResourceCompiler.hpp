#pragma once

#include <expected>
#include <filesystem>
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
// TODO: Find better error names
enum class Error {
  Unknown = 0,
  ShaderSession,
  ShaderModuleCompilation,
  ShaderEntryPointCompilation,
  ShaderEntryPointComposer,
  ShaderEntryPointLinker,
  ShaderEntryPointCodegen,
};

enum class AssetID : u64 { Invalid = ~0_u64 };

enum class ShaderOptimization : i32 {
  None = 0,
  Default,
  High,
  Maximal,
};

struct ShaderSessionInfo {
  ShaderOptimization optimization = ShaderOptimization::Default;
  bool debug_symbols = false;
  std::filesystem::path root_directory = {};
  std::vector<std::pair<std::string, std::string>> definitions = {};
};

struct ShaderInfo {
  std::filesystem::path path = {};
  std::string module_name = {};
  std::vector<std::string> entry_points = {};
};

struct OXRC_API ShaderSession {
  struct Impl;

protected:
  Impl* impl;

public:
  ShaderSession(Impl* impl_) : impl(impl_) {}

  auto compile_shader(const ShaderInfo& info) -> std::expected<AssetID, Error>;
};

struct OXRC_API Session {
  struct Impl;

protected:
  Impl* impl;

public:
  static auto create() -> std::expected<Session, Error>;

  Session(Impl* impl_) : impl(impl_) {}
  auto create_shader_session(const ShaderSessionInfo& info) -> std::expected<ShaderSession, Error>;
};

} // namespace ox::rc
