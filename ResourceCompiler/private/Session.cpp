#include "Session.hpp"

#include <slang-com-ptr.h>
#include <utility>

#include "ShaderSession.hpp"
#include "SlangVFS.hpp"

namespace ox::rc {
auto Session::create() -> std::expected<Session, Error> {
  auto* self = new Session::Impl;
  auto write_lock = std::unique_lock(self->session_mutex);
  slang::createGlobalSession(self->slang_global_session.writeRef());

  return Session(self);
}

auto Session::create_shader_session(const ShaderSessionInfo& info) -> std::expected<ShaderSession, Error> {
  auto debug_level = static_cast<i32>(
    info.debug_symbols ? SLANG_DEBUG_INFO_LEVEL_MAXIMAL : SLANG_DEBUG_INFO_LEVEL_NONE
  );

  slang::CompilerOptionEntry entries[] = {
    {.name = slang::CompilerOptionName::Optimization,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = std::to_underlying(info.optimization)}},
    {.name = slang::CompilerOptionName::DebugInformationFormat,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = SLANG_DEBUG_INFO_FORMAT_DEFAULT}},
    {.name = slang::CompilerOptionName::DebugInformation,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = debug_level}},
    {.name = slang::CompilerOptionName::UseUpToDateBinaryModule,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 1}},
    {.name = slang::CompilerOptionName::GLSLForceScalarLayout,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 1}},
    {.name = slang::CompilerOptionName::Language,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "slang"}},
    {.name = slang::CompilerOptionName::VulkanUseEntryPointName,
     .value = {.kind = slang::CompilerOptionValueKind::Int, .intValue0 = 1}},
    {.name = slang::CompilerOptionName::DisableWarning,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "39001"}},
    {.name = slang::CompilerOptionName::DisableWarning,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "41012"}},
    {.name = slang::CompilerOptionName::DisableWarning,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "41017"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "vk_mem_model"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvGroupNonUniformBallot"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvGroupNonUniformShuffle"}},
    {.name = slang::CompilerOptionName::Capability,
     .value = {.kind = slang::CompilerOptionValueKind::String, .stringValue0 = "spvImageGatherExtended"}}
  };

  auto macros = std::vector<slang::PreprocessorMacroDesc>();
  macros.reserve(info.definitions.size());
  for (const auto& [first, second] : info.definitions) {
    macros.emplace_back(first.c_str(), second.c_str());
  }

  auto target_desc = slang::TargetDesc{
    .format = SLANG_SPIRV,
    .profile = impl->slang_global_session->findProfile("spirv_1_5"),
    .flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY,
    .floatingPointMode = SLANG_FLOATING_POINT_MODE_FAST,
    .lineDirectiveMode = SLANG_LINE_DIRECTIVE_MODE_STANDARD,
    .forceGLSLScalarBufferLayout = true,
    .compilerOptionEntries = entries,
    .compilerOptionEntryCount = static_cast<u32>(count_of(entries)),
  };

  auto slang_fs = std::make_unique<SlangVirtualFS>(info.root_directory);
  const auto search_path = info.root_directory.string();
  const auto* search_path_cstr = search_path.c_str();
  const c8* search_paths[] = {search_path_cstr};
  auto session_desc = slang::SessionDesc{
    .targets = &target_desc,
    .targetCount = 1,
    .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
    .searchPaths = search_paths,
    .searchPathCount = count_of(search_paths),
    .preprocessorMacros = macros.data(),
    .preprocessorMacroCount = static_cast<u32>(macros.size()),
    .fileSystem = slang_fs.get(),
  };

  auto shader_session_handle = std::make_unique<ShaderSession::Impl>();
  shader_session_handle->rc_session = impl;
  shader_session_handle->virtual_fs = std::move(slang_fs);

  {
    auto write_lock = std::unique_lock(impl->session_mutex);
    if (SLANG_FAILED(
          impl->slang_global_session->createSession(session_desc, shader_session_handle->slang_session.writeRef())
        )) {
      return std::unexpected(Error::ShaderSession);
    }

    auto shader_session = ShaderSession(shader_session_handle.get());
    impl->shader_sessions.emplace_back(std::move(shader_session_handle));

    return shader_session;
  }
}

auto Session::create_asset(AssetType type) -> AssetID {
  auto write_lock = std::unique_lock(impl->assets_mutex);

  auto asset_id = impl->assets.create_slot(CompiledAsset{.type = type, .none = 0});
  auto asset_data_index = SlotMap_decode_id(asset_id).index;
  if (asset_data_index >= impl->asset_datas.size()) {
    impl->asset_datas.resize(asset_data_index + 1);
  }

  return asset_id;
}

auto Session::get_asset_data(AssetID asset_id) -> std::span<u8> {
  auto read_lock = std::shared_lock(impl->assets_mutex);
  auto asset_index = SlotMap_decode_id(asset_id).index;

  return impl->asset_datas[asset_index];
}

auto Session::get_shader_asset(AssetID asset_id) -> ShaderAsset {
  auto asset = get_asset(asset_id);

  return asset->shader;
}

auto Session::get_asset(AssetID asset_id) -> Borrowed<CompiledAsset> {
  auto read_lock = std::shared_lock(impl->assets_mutex);
  auto* asset = impl->assets.slot(asset_id);

  return Borrowed(impl->assets_mutex, asset);
}

auto Session::set_asset_data(AssetID asset_id, std::vector<u8> asset_data) -> void {
  auto read_lock = std::shared_lock(impl->assets_mutex);
  auto asset_index = SlotMap_decode_id(asset_id).index;
  impl->asset_datas[asset_index] = std::move(asset_data);
}

auto Session::set_asset_info(AssetID asset_id, ShaderAsset shader_asset) -> void {
  auto asset = get_asset(asset_id);
  asset->shader = shader_asset;
}

} // namespace ox::rc
