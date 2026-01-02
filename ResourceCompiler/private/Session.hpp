#pragma once

#include <ankerl/unordered_dense.h>
#include <shared_mutex>
#include <slang-com-ptr.h>

#include "Asset/AssetFile.hpp"
#include "Core/UUID.hpp"
#include "Memory/SlotMap.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct CompiledAsset {
  UUID uuid = {};
  AssetType type = AssetType::None;
  union {
    u32 none = 0;
    ShaderAssetEntry shader;
    ModelAssetEntry model;
  };
};

struct Session::Impl {
  u16 version = 10;
  std::shared_mutex messages_mutex = {};
  std::vector<std::string> messages = {};

  std::shared_mutex session_mutex = {};
  Slang::ComPtr<slang::IGlobalSession> slang_global_session = {};

  std::shared_mutex assets_mutex = {};
  SlotMap<CompiledAsset, AssetID> assets = {};
  std::vector<std::vector<u8>> asset_datas = {};
  ankerl::unordered_dense::map<std::filesystem::path, CacheEntry> asset_cache = {};

  std::vector<CompileRequest> compile_requests = {};
};

} // namespace ox::rc
