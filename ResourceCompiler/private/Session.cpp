#include "Session.hpp"

#include <Memory/Hasher.hpp>
#include <OS/File.hpp>
#include <Utils/JsonWriter.hpp>
#include <fmt/format.h>
#include <fmt/std.h>
#include <simdjson.h>
#include <slang-com-ptr.h>
#include <utility>
#include <xxhash.h>

#include "ShaderSession.hpp"

namespace ox::rc {
struct RCSlangBlob : public ISlangBlob {
  std::vector<u8> m_data = {};
  std::atomic_uint32_t m_refCount = 1;

  SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** outObject) SLANG_OVERRIDE {
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++m_refCount; }

  SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
    --m_refCount;
    if (m_refCount == 0) {
      delete this;
      return 0;
    }
    return m_refCount;
  }

  RCSlangBlob(std::vector<u8> data) : m_data(std::move(data)) {}
  virtual ~RCSlangBlob() = default;
  SLANG_NO_THROW const void* SLANG_MCALL getBufferPointer() final { return m_data.data(); };
  SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() final { return m_data.size(); };
};

struct RCSlangFileSystem : public ISlangFileSystem {
  // slang internally handles caching, hopefully this vector will only contain unique paths
  std::vector<std::filesystem::path> accessed_module_paths = {};

  RCSlangFileSystem(std::vector<std::filesystem::path> accessed_module_paths_)
      : accessed_module_paths(std::move(accessed_module_paths_)) {};

  SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** outObject) SLANG_OVERRIDE {
    return SLANG_E_NO_INTERFACE;
  }

  SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID&) final { return nullptr; }
  SLANG_NO_THROW uint32_t SLANG_MCALL addRef() SLANG_OVERRIDE { return 1; }
  SLANG_NO_THROW uint32_t SLANG_MCALL release() SLANG_OVERRIDE { return 1; }
  SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char* path_cstr, ISlangBlob** outBlob) SLANG_OVERRIDE {
    auto path = std::filesystem::path(path_cstr);
    const auto result = File::to_bytes(path);
    if (!result.empty()) {
      accessed_module_paths.push_back(path);
      *outBlob = new RCSlangBlob(std::move(result));

      return SLANG_OK;
    }

    return SLANG_E_NOT_FOUND;
  }
};

auto create_shader_session(
  slang::IGlobalSession* global_session, const ShaderSessionInfo& info, RCSlangFileSystem* file_system
) -> Slang::ComPtr<slang::ISession> {
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
    .profile = global_session->findProfile("spirv_1_5"),
    .flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY,
    .floatingPointMode = SLANG_FLOATING_POINT_MODE_FAST,
    .lineDirectiveMode = SLANG_LINE_DIRECTIVE_MODE_STANDARD,
    .forceGLSLScalarBufferLayout = true,
    .compilerOptionEntries = entries,
    .compilerOptionEntryCount = static_cast<u32>(count_of(entries)),
  };

  auto root_dir = info.root_directory.lexically_normal();
  const auto search_path = root_dir.string();
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
    .fileSystem = file_system,
  };

  auto session = Slang::ComPtr<slang::ISession>{};
  if (SLANG_FAILED(global_session->createSession(session_desc, session.writeRef()))) {
    return nullptr;
  }

  return session;
}

auto read_shader_session_meta(
  Session self, simdjson::ondemand::value& json, const std::filesystem::path& meta_file_path
) -> option<ShaderCompileRequest> {
  auto compile_request = ShaderCompileRequest{};
  auto& shader_session_info = compile_request.session_info;

  auto session_name_json = json["session_name"];
  if (session_name_json.error() || !session_name_json.is_string()) {
    self.push_error("Shader session meta is missing `session_name`.");
    return nullopt;
  }
  auto session_name_str = session_name_json.get_string().value_unsafe();
  shader_session_info.name = session_name_str;

  auto root_directory_json = json["root_directory"];
  if (root_directory_json.error() || !root_directory_json.is_string()) {
    self.push_error("Shader session meta is missing `root_director`.");
  }
  auto root_directory_str = root_directory_json.get_string().value_unsafe();
  shader_session_info.root_directory = (meta_file_path / root_directory_str).make_preferred().lexically_normal();

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
        return nullopt;
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
        return nullopt;
      }
      shader_info.module_name = name_json.get_string().value_unsafe();

      auto path_json = program_json["path"];
      if (path_json.error() || !path_json.is_string()) {
        self.push_error(
          fmt::format("Shader module '{}::{}' is missing a program path.", session_name_str, shader_info.module_name)
        );
        return nullopt;
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
        return nullopt;
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
          return nullopt;
        }

        auto entry_point = entry_point_json.get_string().value_unsafe();
        shader_info.entry_points.push_back(std::string(entry_point));
      }

      compile_request.shader_infos.emplace_back(std::move(shader_info));
    }
  }

  return compile_request;
}

auto Session::create(u16 version) -> Session {
  auto* self = new Session::Impl;
  self->version = version;
  auto write_lock = std::unique_lock(self->session_mutex);
  slang::createGlobalSession(self->slang_global_session.writeRef());

  return Session(self);
}

auto Session::create(std::span<std::filesystem::path> meta_paths) -> Session {
  auto self = Session::create(0);
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

  auto version_json = json["version"];
  if (version_json.error() || !version_json.is_integer()) {
    push_error(fmt::format("An error occured while reading meta file {}. Missing/wrong `version` field!", path));
    return false;
  }

  auto version = version_json.get_uint64().value_unsafe();
  impl->version = static_cast<u16>(version);

  auto shader_sessions_json = json["shader_sessions"];
  if (!shader_sessions_json.error()) {
    for (auto v : shader_sessions_json.get_array()) {
      auto request = read_shader_session_meta(impl, v.value_unsafe(), path.parent_path());
      if (!request.has_value()) {
        return false;
      }

      this->add_request(request.value());
    }
  }

  auto models_json = json["models"];
  if (!models_json.error()) {
    for (auto v : models_json.get_array()) {
      auto path_json = v["path"].get_string();
      if (path_json.error()) {
        push_error(
          fmt::format(
            "An error occured while reading meta file {}. {}",
            path,
            simdjson::error_message(path_json.error())
          )
        );
        return false;
      }

      auto is_foliage = false;
      auto is_foliage_json = v["is_foliage"].get_bool();
      if (!is_foliage_json.error()) {
        is_foliage = is_foliage_json.value_unsafe();
      }

      auto model_path = std::filesystem::path(path_json.value_unsafe());
      model_path = (path.parent_path() / model_path).make_preferred().lexically_normal();
      this->add_request(ModelCompileRequest{.path = model_path, .is_foliage = is_foliage});
    }
  }

  return true;
}

auto Session::import_cache(const std::filesystem::path& path) -> void {
  if (!std::filesystem::exists(path)) {
    return;
  }

  auto cache_str = File::to_string(path);
  if (cache_str.empty()) {
    push_error(fmt::format("An error occured while reading cache file {}.", path));
    return;
  }

  auto parser = simdjson::ondemand::parser();
  auto json_str = simdjson::padded_string(cache_str);
  auto json = parser.iterate(json_str);
  if (json.error()) {
    push_message(
      fmt::format("Cache file does not exist, recreating one {}. {}", path, simdjson::error_message(json.error()))
    );
  }

  auto entries_json = json["entries"];
  if (entries_json.error()) {
    push_error(
      fmt::format("An error occured while reading cache file. {}", simdjson::error_message(entries_json.error()))
    );
    return;
  }

  for (auto entry_json : entries_json.get_array()) {
    auto source_path_json = entry_json["source_path"].get_string();
    if (source_path_json.error()) {
      push_error(
        fmt::format("An error occured while reading cache file. {}", simdjson::error_message(source_path_json.error()))
      );
      return;
    }
    auto source_path = std::filesystem::path(source_path_json.value_unsafe());

    auto source_uuid_json = entry_json["source_uuid"].get_string();
    if (source_uuid_json.error()) {
      push_error(
        fmt::format("An error occured while reading cache file. {}", simdjson::error_message(source_uuid_json.error()))
      );
      return;
    }
    auto source_uuid = UUID::from_string(source_uuid_json.value_unsafe());
    if (!source_uuid.has_value()) {
      push_error(fmt::format("An error occured while reading cache file. Broken UUID."));
      return;
    }

    auto source_hash_json = entry_json["source_hash"].get_uint64();
    if (source_hash_json.error()) {
      push_error(
        fmt::format("An error occured while reading cache file. {}", simdjson::error_message(source_hash_json.error()))
      );
      return;
    }

    auto dependencies = std::vector<std::filesystem::path>{};
    auto dependencies_json = entry_json["dependencies"].get_array();
    for (auto dependency_json : dependencies_json) {
      auto dependency_path_json = dependency_json.get_string();
      if (dependency_path_json.error()) {
        push_error(
          fmt::format(
            "An error occured while reading cache file. {}",
            simdjson::error_message(dependency_path_json.error())
          )
        );
        return;
      }
      dependencies.push_back(std::filesystem::path(dependency_path_json.value_unsafe()));
    }

    this->cache_asset(source_path, {source_uuid.value(), source_hash_json.value_unsafe(), std::move(dependencies)});
  }
}

auto Session::save_cache(const std::filesystem::path& path) -> void {
  auto read_lock = std::shared_lock(impl->assets_mutex);

  auto json = JsonWriter{};
  json.begin_obj();
  json["entries"].begin_array();
  for (auto& [source_path, cache_entry] : impl->asset_cache) {
    json.begin_obj();
    json["source_path"] = source_path;
    json["source_uuid"] = cache_entry.source_uuid.str();
    json["source_hash"] = cache_entry.source_hash;
    json["dependencies"].begin_array();
    for (auto& dependency : cache_entry.dependencies) {
      json << dependency;
    }
    json.end_array();
    json.end_obj();
  }
  json.end_array();
  json.end_obj();

  auto file = File(path, FileAccess::Write);
  file.write(json.stream.view());
  file.close();
}

auto Session::output_to(const std::filesystem::path& path) -> void {
  auto ec = std::error_code{};
  if (std::filesystem::create_directories(path, ec); ec) {
    push_error(fmt::format("Failed to output resources, could not create directory for {}: {}", path, ec.message()));
    return;
  }

  // at this point we dont care about concurrency, so lock the entire function
  auto read_lock = std::shared_lock(impl->assets_mutex);
  auto assets = impl->assets.slots_unsafe();

  auto flags = AssetFileFlags::None;
  // if (impl->pack) {
  //   flags |= AssetFileFlags::Packed;
  // }

  auto write_header = [&](File& writer) {
    writer.write_data("OXRC", 4);
    writer.write_trivial(impl->version);
    writer.write_trivial(flags);
    writer.write_trivial(static_cast<u32>(assets.size()));
  };

  auto write_asset = [&](File& writer, CompiledAsset& asset, u32 asset_data_size, u32 header_data_offset) {
    writer.write(asset.uuid.bytes());
    writer.write_trivial(asset_data_size);
    writer.write_trivial(header_data_offset);
    writer.write_trivial(asset.type);
    switch (asset.type) {
      case AssetType::Shader: {
        writer.write_trivial(asset.shader.entry_points);
      } break;
      case AssetType::Model: {
        writer.write_trivial(asset.model.nodes);
        writer.write_trivial(asset.model.meshes);
      } break;
      default:;
    }
  };

  for (const auto& [asset, asset_data] : std::views::zip(assets, impl->asset_datas)) {
    auto uuid_str = asset.uuid.str();
    auto asset_path = path / uuid_str;
    auto writer = File(asset_path, FileAccess::Write);
    write_header(writer);
    write_asset(writer, asset, asset_data.size(), 0);
    writer.write(asset_data);
  }
}

auto Session::add_request(const CompileRequest& request) -> void { impl->compile_requests.emplace_back(request); }

auto Session::compile_requests() -> bool {
  auto check_cached_asset = [&](const std::filesystem::path& path) {
    auto source_hash = this->hash_file(path);
    auto uuid = UUID{};
    auto should_build = true;

    const auto cached_asset_it = impl->asset_cache.find(path);
    if (cached_asset_it != impl->asset_cache.end()) {
      const auto& cached_asset = cached_asset_it->second;
      if (source_hash == cached_asset.source_hash) {
        should_build = false;
      }

      uuid = cached_asset.source_uuid;
    } else {
      uuid = UUID::generate_random();
    }

    return std::make_tuple(should_build, uuid, source_hash);
  };

  for (const auto& request : impl->compile_requests) {
    std::visit(
      ox::match{
        [](const auto&) {},
        [&](const ShaderCompileRequest& shader_request) {
          const auto& [session_info, shader_infos] = shader_request;

          // Shader sessions are special, if one of the included modules is changed
          // we should rebuild entire shader session
          auto session_dependencies = std::vector<std::filesystem::path>{};
          auto build_entire_session = false;
          auto cached_session_it = impl->asset_cache.find(session_info.root_directory);
          if (cached_session_it != impl->asset_cache.end()) {
            auto& session_entry = cached_session_it->second;
            session_dependencies = session_entry.dependencies;

            for (const auto& module_path : session_entry.dependencies) {
              auto [should_build, uuid, source_hash] = check_cached_asset(module_path);
              if (should_build) {
                build_entire_session = true;
                // something changed with dependencies, we are going to rebuild it
                // with proper dependencies this time
                session_dependencies.clear();
                break;
              }
            }
          }

          auto session_fs = RCSlangFileSystem(std::move(session_dependencies));
          auto slang_session = create_shader_session(impl->slang_global_session.get(), session_info, &session_fs);
          if (!slang_session) {
            return;
          }

          auto shader_session = ShaderSession{
            .rc_session = impl,
            .name = session_info.name,
            .slang_session = slang_session.get(),
            .root_dir = session_info.root_directory,
          };

          for (const auto& info : shader_request.shader_infos) {
            const auto path = (shader_request.session_info.root_directory / info.path).lexically_normal();
            auto [should_build, uuid, source_hash] = check_cached_asset(path);
            if (!should_build && !build_entire_session) {
              continue;
            }

            auto result = shader_session.compile_shader(info);
            if (!result.has_value()) {
              continue;
            }

            this->push_message(fmt::format("Compiled shader {}", info.module_name));

            auto [shader_asset, asset_data] = std::move(result.value());
            auto asset_id = this->create_asset(uuid, AssetType::Shader);
            this->set_asset_info(asset_id, shader_asset);
            this->set_asset_data(asset_id, std::move(asset_data));
            this->cache_asset(path, {.source_uuid = uuid, .source_hash = source_hash});
          }

          for (const auto& module_path : session_fs.accessed_module_paths) {
            auto source_hash = this->hash_file(module_path);
            this->cache_asset(module_path, {.source_uuid = {}, .source_hash = source_hash});
          }

          this->cache_asset(
            shader_session.root_dir,
            {.source_uuid = {}, .source_hash = 0, .dependencies = std::move(session_fs.accessed_module_paths)}
          );
        },
        [&](const ModelCompileRequest& model_request) {
          const auto path = model_request.path.lexically_normal();
          const auto cached_asset_it = impl->asset_cache.find(path);
          auto uuid = UUID{};
          if (cached_asset_it != impl->asset_cache.end()) {
            const auto& cached_asset = cached_asset_it->second;
            auto last_source_hash = this->hash_file(path);
            if (last_source_hash == cached_asset.source_hash) {
              return;
            }

            uuid = cached_asset.source_uuid;
          } else {
            uuid = UUID::generate_random();
          }

          // process_model(impl, request);
        },
      },
      request
    );
  }

  impl->compile_requests.clear();

  return true;
}

auto Session::create_asset(const UUID& uuid, AssetType type) -> AssetID {
  auto write_lock = std::unique_lock(impl->assets_mutex);

  auto asset_id = impl->assets.create_slot(CompiledAsset{.uuid = uuid, .type = type, .none = 0});
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

auto Session::get_shader_asset(AssetID asset_id) -> ShaderAssetEntry {
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

auto Session::set_asset_info(AssetID asset_id, const ShaderAssetEntry& shader_asset) -> void {
  auto asset = get_asset(asset_id);
  asset->shader = shader_asset;
}

auto Session::set_asset_info(AssetID asset_id, const ModelAssetEntry& model_asset) -> void {
  auto asset = get_asset(asset_id);
  asset->model = model_asset;
}

auto Session::hash_file(const std::filesystem::path& path) -> u64 {
  auto file = File(path, FileAccess::Read);
  auto* mapped_data = file.map();

  return XXH64(mapped_data, file.size, 0);
}

auto Session::hash_data(void* data, usize size_bytes) -> u64 { return XXH64(data, size_bytes, 0); }

auto Session::cache_asset(const std::filesystem::path& path, const CacheEntry& entry) -> void {
  auto write_lock = std::unique_lock(impl->assets_mutex);
  impl->asset_cache[path] = entry;
}

} // namespace ox::rc
