#pragma once

#include <ankerl/unordered_dense.h>
#include <shared_mutex>
#include <slang-com-ptr.h>

#include "Asset/AssetFile.hpp"
#include "Core/UUID.hpp"
#include "Memory/SlotMap.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct ShaderCompileRequest {
  ShaderSessionInfo session_info = {};
  std::vector<ShaderInfo> shader_infos = {};
};

struct ModelProcessRequest {
  std::filesystem::path path = {};
  bool is_foliage = false;
};

struct CompiledAsset {
  UUID uuid = {};
  AssetType type = AssetType::None;
  union {
    u32 none = 0;
    ShaderAssetEntry shader;
    ModelAssetEntry model;
  };
};

// Defined in ModelProcessor.cpp
auto process_model(Session self, const ModelProcessRequest &request) -> AssetID;

struct Session::Impl {
  u16 version = 10;
  std::shared_mutex messages_mutex = {};
  std::vector<std::string> messages = {};

  std::shared_mutex session_mutex = {};
  Slang::ComPtr<slang::IGlobalSession> slang_global_session = {};
  std::vector<std::unique_ptr<ShaderSession::Impl>> shader_sessions = {};

  std::shared_mutex assets_mutex = {};
  SlotMap<CompiledAsset, AssetID> assets = {};
  std::vector<std::vector<u8>> asset_datas = {};
  ankerl::unordered_dense::map<std::filesystem::path, u64> asset_file_times = {};

  std::vector<ShaderCompileRequest> shader_compile_requests = {};
  std::vector<ModelProcessRequest> model_process_requests = {};

  bool pack = false;
};

} // namespace ox::rc
