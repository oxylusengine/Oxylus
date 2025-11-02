#include "Session.hpp"

#include <fmt/format.h>
#include <fmt/std.h>
#include <simdjson.h>
#include <slang-com-ptr.h>
#include <utility>

#include "Memory/Hasher.hpp"
#include "ShaderSession.hpp"
#include "SlangVFS.hpp"

namespace ox::rc {
auto read_shader_session_meta(Session::Impl* impl, simdjson::ondemand::value& json) -> bool {
  auto self = Session(impl);
  auto compile_request = ShaderCompileRequest{};
  auto& shader_session_info = compile_request.session_info;

  auto session_name_json = json["session_name"];
  if (session_name_json.error() || !session_name_json.is_string()) {
    self.push_error("Shader session meta is missing `session_name`.");
    return false;
  }
  auto session_name_str = session_name_json.get_string().value_unsafe();

  auto root_directory_json = json["root_directory"];
  if (root_directory_json.error() || !root_directory_json.is_string()) {
    self.push_error("Shader session meta is missing `root_director`.");
  }
  auto root_directory_str = root_directory_json.get_string().value_unsafe();
  shader_session_info.root_directory = std::filesystem::path(root_directory_str).make_preferred();

  auto optimization_json = json["optimization"];
  if (auto optimization_str = optimization_json.get_string(); optimization_str.has_value()) {
    shader_session_info.optimization = [&] {
      switch (fnv64_str(optimization_str.value_unsafe())) {
        case fnv64_c("none")   : return ShaderOptimization::None;
        case fnv64_c("default"): return ShaderOptimization::Default;
        case fnv64_c("high")   : return ShaderOptimization::High;
        case fnv64_c("maximal"): return ShaderOptimization::Maximal;
        default                : {
          self.push_error(
            fmt::format(
              "Shader session '{}' has unknown optimization level '{}', falling back to default.",
              session_name_str,
              optimization_str.value_unsafe()
            )
          );
          return ShaderOptimization::Default;
        }
      }
    }();
  }

  auto debug_symbols_json = json["debug_symbols"];
  if (auto debug_symbols = debug_symbols_json.get_bool(); debug_symbols.has_value()) {
    shader_session_info.debug_symbols = debug_symbols.value_unsafe();
  }

  auto definitions_json = json["definitions"];
  if (!definitions_json.error()) {
    for (auto v : definitions_json.get_array()) {
      auto name_json = v["name"];
      auto value_json = v["value"];
      if ((name_json.error() && name_json.is_string()) || value_json.error()) {
        self.push_error(fmt::format("Shader session '{}' has invalid definition.", session_name_str));
        return false;
      }
      auto name_str = name_json.get_string().value_unsafe();
      auto value_str = value_json.raw_json_token().value_unsafe();

      shader_session_info.definitions.emplace_back(name_str, value_str);
    }
  }

  auto programs_json = json["programs"];
  if (!programs_json.error()) {
    for (auto program_json : programs_json.get_array()) {
      auto shader_info = ShaderInfo{};

      auto name_json = program_json["name"];
      if (name_json.error() || !name_json.is_string()) {
        self.push_error(fmt::format("Shader session '{}' is missing a program name.", session_name_str));
        return false;
      }
      shader_info.module_name = name_json.get_string().value_unsafe();

      auto path_json = program_json["path"];
      if (path_json.error() || !path_json.is_string()) {
        self.push_error(
          fmt::format("Shader module '{}::{}' is missing a program path.", session_name_str, shader_info.module_name)
        );
        return false;
      }
      shader_info.path = std::filesystem::path(path_json.get_string().value_unsafe()).make_preferred();

      auto entry_points_json = program_json["entry_points"];
      if (entry_points_json.error()) {
        self.push_error(
          fmt::format(
            "Shader module '{}::{}' is missing an array of entry points.",
            session_name_str,
            shader_info.module_name
          )
        );
        return false;
      }

      for (auto entry_point_json : entry_points_json.get_array()) {
        if (entry_point_json.error() || !entry_point_json.is_string()) {
          self.push_error(
            fmt::format(
              "Entry point defined at shader module '{}::{}' needs to be a  valid string.",
              session_name_str,
              shader_info.module_name
            )
          );
          return false;
        }
      }

      compile_request.shader_infos.emplace_back(std::move(shader_info));
    }
  }

  impl->shader_compile_requests.emplace_back(std::move(compile_request));

  return true;
}

auto Session::create() -> Session {
  auto* self = new Session::Impl;
  auto write_lock = std::unique_lock(self->session_mutex);
  slang::createGlobalSession(self->slang_global_session.writeRef());

  return Session(self);
}

auto Session::create(std::span<std::filesystem::path> meta_paths) -> Session {
  auto self = Session::create();
  if (!self) {
    return nullptr;
  }

  for (const auto& path : meta_paths) {
    if (!self.import_meta(path)) {
      self.destroy();
      return nullptr;
    }
  }

  return self;
}

auto Session::destroy() -> void { delete impl; }

auto Session::import_meta(const std::filesystem::path& path) -> bool {
  auto meta_str = File::to_string(path);
  if (meta_str.empty()) {
    push_error(fmt::format("An error occured while reading meta file {}.", path));
    return false;
  }

  auto parser = simdjson::ondemand::parser();
  auto json_str = simdjson::padded_string(meta_str);
  auto json = parser.iterate(json_str);
  if (json.error()) {
    push_error(
      fmt::format("An error occured while reading meta file {}. {}", path, simdjson::error_message(json.error()))
    );
    return false;
  }

  auto asset_type_json = json["type"];
  if (asset_type_json.error() || !asset_type_json.is_string()) {
    push_error(fmt::format("An error occured while reading meta file {}. Missing/wrong `type` field!", path));
    return false;
  }

  auto asset_type_str = asset_type_json.get_string().value_unsafe();
  switch (fnv64_str(asset_type_str)) {
    case fnv64_c("shader"): {
      auto shader_sessions_json = json["shader_sessions"];
      if (shader_sessions_json.error()) {
        push_error(
          fmt::format("An error occured while reading meta file {}. Missing/wrong `shader_sessions` field!", path)
        );
        return false;
      }

      for (auto v : shader_sessions_json.get_array()) {
        if (!read_shader_session_meta(impl, v.value_unsafe())) {
          return false;
        }
      }
    } break;
    default: {
      push_error(
        fmt::format("An error occured while reading meta file {}. Undefined asset type {}.", path, asset_type_str)
      );
      return false;
    }
  }

  return true;
}

auto Session::create_shader_session(const ShaderSessionInfo& info) -> ShaderSession {
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
  shader_session_handle->name = info.name;
  shader_session_handle->rc_session = impl;
  shader_session_handle->virtual_fs = std::move(slang_fs);

  {
    auto write_lock = std::unique_lock(impl->session_mutex);
    if (SLANG_FAILED(
          impl->slang_global_session->createSession(session_desc, shader_session_handle->slang_session.writeRef())
        )) {
      push_error(fmt::format("Failed to create slang session named {}", info.name));
      return nullptr;
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

auto Session::push_error(std::string str, std::source_location LOC) -> void {
  auto write_lock = std::unique_lock(impl->messages_mutex);
  impl->messages.emplace_back(fmt::format("[ERROR] {}:{}: {}", LOC.file_name(), LOC.line(), std::move(str)));
}

auto Session::push_message(std::string str, std::source_location LOC) -> void {
  auto write_lock = std::unique_lock(impl->messages_mutex);
  impl->messages.emplace_back(fmt::format("{}:{}: {}", LOC.file_name(), LOC.line(), std::move(str)));
}

auto Session::get_messages() -> std::vector<std::string> {
  auto write_lock = std::unique_lock(impl->messages_mutex);
  return std::move(impl->messages);
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
