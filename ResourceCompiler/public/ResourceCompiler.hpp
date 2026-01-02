#pragma once

#include <filesystem>
#include <source_location>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"
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

struct CacheEntry {
  UUID source_uuid = UUID(nullptr);
  u64 source_hash = 0;
  std::vector<std::filesystem::path> dependencies = {};
};

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

struct ModelCompileRequest {
  std::filesystem::path path = {};
  bool is_foliage = false;
};

using CompileRequest = std::variant<ShaderCompileRequest, ModelCompileRequest>;

struct CompiledAsset;
struct OXRC_API Session {
  struct Impl;

protected:
  Impl* impl;

  constexpr auto& operator->() { return *impl; }

public:
  static auto create(u16 version) -> Session;
  static auto create(std::span<std::filesystem::path> meta_paths) -> Session;
  auto destroy() -> void;

  Session() = default;
  Session(Impl* impl_) : impl(impl_) {}
  explicit operator bool() const { return impl; }

  auto import_meta(const std::filesystem::path& path) -> bool;
  auto import_cache(const std::filesystem::path& path) -> void;
  auto save_cache(const std::filesystem::path& path) -> void;
  auto output_to(const std::filesystem::path& path) -> void;

  auto add_request(const CompileRequest& request) -> void;
  auto compile_requests() -> bool;

  auto create_asset(const UUID& uuid, AssetType type) -> AssetID;
  auto get_asset_data(AssetID asset_id) -> std::span<u8>;
  auto get_shader_asset(AssetID asset_id) -> ShaderAssetEntry;

  auto push_error(std::string str, std::source_location LOC = std::source_location::current()) -> void;
  auto push_message(std::string str, std::source_location LOC = std::source_location::current()) -> void;
  auto get_messages() -> std::vector<std::string>;

  auto get_asset(AssetID asset_id) -> Borrowed<CompiledAsset>;
  auto set_asset_data(AssetID asset_id, std::vector<u8> asset_data) -> void;
  auto set_asset_info(AssetID asset_id, const ShaderAssetEntry& shader_asset) -> void;
  auto set_asset_info(AssetID asset_id, const ModelAssetEntry& model_asset) -> void;

  auto hash_file(const std::filesystem::path& path) -> u64;
  auto hash_data(void* data, usize size_bytes) -> u64;
  auto cache_asset(const std::filesystem::path& path, const CacheEntry& entry) -> void;
};

} // namespace ox::rc
