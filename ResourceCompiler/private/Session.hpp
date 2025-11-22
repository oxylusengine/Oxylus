#pragma once

#include <ankerl/unordered_dense.h>
#include <shared_mutex>
#include <slang-com-ptr.h>

#include "Asset/AssetFile.hpp"
#include "Memory/SlotMap.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
struct ShaderCompileRequest {
  ShaderSessionInfo session_info = {};
  std::vector<ShaderInfo> shader_infos = {};
};

struct CompiledAsset {
  AssetType type = AssetType::None;
  union {
    u32 none = 0;
    ShaderAsset shader;
  };
};

struct Session::Impl {
  std::shared_mutex messages_mutex = {};
  std::vector<std::string> messages = {};

  std::shared_mutex session_mutex = {};
  Slang::ComPtr<slang::IGlobalSession> slang_global_session = {};
  std::vector<std::unique_ptr<ShaderSession::Impl>> shader_sessions = {};

  // TODO: Replace this with a proper allocator
  std::shared_mutex assets_mutex = {};
  SlotMap<CompiledAsset, AssetID> assets = {};
  std::vector<std::vector<u8>> asset_datas = {};

  std::vector<ShaderCompileRequest> shader_compile_requests = {};
};

} // namespace ox::rc
