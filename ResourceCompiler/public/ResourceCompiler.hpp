#pragma once

#include <filesystem>
#include <source_location>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Core/Types.hpp"
#include "Memory/Borrowed.hpp"

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
enum class AssetID : u64 { Invalid = ~0_u64 };

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

struct OXRC_API ShaderSession {
  struct Impl;

protected:
  Impl* impl;

public:
  ShaderSession(Impl* impl_) : impl(impl_) {}
  explicit operator bool() const { return impl; }

  auto compile_shader(const ShaderInfo& info) -> AssetID;
};

struct CompiledAsset;
struct OXRC_API Session {
  struct Impl;

protected:
  Impl* impl;

public:
  static auto create() -> Session;
  static auto create(std::span<std::filesystem::path> meta_paths) -> Session;
  auto destroy() -> void;

  Session() = default;
  Session(Impl* impl_) : impl(impl_) {}
  explicit operator bool() const { return impl; }

  auto import_meta(const std::filesystem::path& path) -> bool;

  auto create_shader_session(const ShaderSessionInfo& info) -> ShaderSession;
  auto compile_requests() -> bool;

  auto create_asset(AssetType type) -> AssetID;
  auto get_asset_data(AssetID asset_id) -> std::span<u8>;
  auto get_shader_asset(AssetID asset_id) -> ShaderAsset;

  auto push_error(std::string str, std::source_location LOC = std::source_location::current()) -> void;
  auto push_message(std::string str, std::source_location LOC = std::source_location::current()) -> void;
  auto get_messages() -> std::vector<std::string>;

protected:
  auto get_asset(AssetID asset_id) -> Borrowed<CompiledAsset>;
  auto set_asset_data(AssetID asset_id, std::vector<u8> asset_data) -> void;
  auto set_asset_info(AssetID asset_id, ShaderAsset shader_asset) -> void;

  friend ShaderSession;
};

} // namespace ox::rc
