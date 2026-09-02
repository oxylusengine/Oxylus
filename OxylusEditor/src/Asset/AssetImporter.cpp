#include "Asset/AssetImporter.hpp"

#include <ResourceCompiler.hpp>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <ranges>
#include <span>

#include "Asset/AssetManager.hpp"
#include "Asset/AssetMeta.hpp"
#include "Core/App.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"

namespace ox {
// A parallel scan can reach one file from several threads at once -- two models naming the same
// sibling texture, or a source and its own `.oxasset` sitting side by side in one directory. Whoever
// claims a path first does the work and the others wait, so nothing is compiled twice or handed two
// UUIDs. The waiter then takes the warm path, which is why no result needs caching here.
//
// Claims nest (a model claims itself, then its textures) but never cycle: a texture import cannot
// reach a model.
struct ImportGate {
  std::mutex mutex = {};
  std::condition_variable released = {};
  ankerl::unordered_dense::set<std::string> in_flight = {};
};

static ImportGate import_gate = {};

struct ImportClaim {
  std::string key = {};

  explicit ImportClaim(const std::filesystem::path& path) : key(path.lexically_normal().string()) {
    auto lock = std::unique_lock(import_gate.mutex);
    import_gate.released.wait(lock, [&] { return !import_gate.in_flight.contains(key); });
    import_gate.in_flight.emplace(key);
  }

  ~ImportClaim() {
    {
      auto lock = std::unique_lock(import_gate.mutex);
      import_gate.in_flight.erase(key);
    }

    import_gate.released.notify_all();
  }

  ImportClaim(const ImportClaim&) = delete;
  auto operator=(const ImportClaim&) -> ImportClaim& = delete;
};

// What the sidecar records for one of a model's textures. Exactly one of the two paths is set:
// `external` for a sibling file that is an asset in its own right, `cache` for one the compiler
// produced and only this model refers to. `is_srgb` is the colour space the glTF's material graph
// asked for, kept because a re-registration has to repeat the directive the compile made.
struct ImportedTexture {
  UUID uuid = UUID(nullptr);
  std::filesystem::path external = {};
  std::string cache = {};
  bool is_srgb = true;
};

struct ImportedModelMeta {
  UUID uuid = UUID(nullptr);
  u64 source_hash = 0;
  std::vector<ImportedTexture> textures = {};
  std::vector<UUID> material_uuids = {};
  std::vector<Material> materials = {};
};

auto needs_compiling(AssetFileType file_type) -> bool {
  switch (file_type) {
    case AssetFileType::GLB :
    case AssetFileType::GLTF:
    case AssetFileType::KTX2:
    case AssetFileType::DDS :
    case AssetFileType::PNG :
    case AssetFileType::JPEG: return true;
    default                 : return false;
  }
}

auto needs_compiling(const std::filesystem::path& path) -> bool { return needs_compiling(to_asset_file_type(path)); }

auto to_asset_type(AssetFileType file_type) -> AssetType {
  switch (file_type) {
    case AssetFileType::GLB       :
    case AssetFileType::GLTF      : return AssetType::Model;
    case AssetFileType::PNG       :
    case AssetFileType::JPEG      :
    case AssetFileType::DDS       :
    case AssetFileType::KTX2      : return AssetType::Texture;
    case AssetFileType::LUA       : return AssetType::Script;
    case AssetFileType::OXTERRAIN : return AssetType::Terrain;
    case AssetFileType::OXPARTICLE: return AssetType::ParticleSystem;
    default                       : return AssetType::None;
  }
}

auto cache_dir() -> std::filesystem::path { return std::filesystem::current_path() / ".oxeditor/assets"; }

auto cache_path(const UUID& uuid, std::string_view extension) -> std::filesystem::path {
  return cache_dir() / (uuid.str() + std::string(extension));
}

auto source_hash(const std::filesystem::path& path) -> u64 {
  ZoneScoped;
  memory::ScopedStack stack;

  auto error = std::error_code{};
  const auto last_write = std::filesystem::last_write_time(path, error);
  if (error) {
    return 0;
  }

  const auto signature = stack.format(
    "{}{}{}{}",
    path.string(),
    last_write.time_since_epoch().count(),
    ASSET_COMPILER_VERSION,
    AssetFileHeader::VERSION
  );

  return fnv64_str(signature);
}

auto hash_to_string(u64 hash) -> std::string { return fmt::format("{:016X}", hash); }

auto mix_hash(u64 hash, u64 value) -> u64 { return (hash ^ value) * 0x100000001B3_u64; }

// `Session` accumulates messages for its whole lifetime, and imports run concurrently, so a compile
// drains what is there rather than slicing by offset -- with several importers pushing at once an
// offset reports another thread's messages too, and reports them again from its own log. Draining
// makes attribution approximate but prints every message exactly once.
struct SessionLog {
  rc::Session& session;

  explicit SessionLog(rc::Session& session_) : session(session_) {}

  ~SessionLog() {
    const auto diagnostics = session.take_diagnostics();
    for (const auto& message : diagnostics.messages) {
      OX_LOG_INFO("{}", message);
    }
    for (const auto& error : diagnostics.errors) {
      OX_LOG_ERROR("{}", error);
    }
  }
};

auto string_to_hash(std::string_view text) -> u64 {
  auto value = 0_u64;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{}) {
    return 0;
  }

  return value;
}

auto write_texture_pack(const std::filesystem::path& path, TextureData&& data, const UUID& uuid) -> bool {
  auto file = AssetFile{};
  file.add_entry(std::move(data), PackedUUID::pack(uuid));

  return file.pack(path);
}

auto read_model_meta(const std::filesystem::path& meta_path) -> option<ImportedModelMeta> {
  ZoneScoped;

  auto meta_json = read_meta_file(meta_path);
  if (!meta_json) {
    return nullopt;
  }

  auto meta = ImportedModelMeta{};

  auto uuid_json = meta_json->doc["uuid"].get_string();
  if (uuid_json.error()) {
    return nullopt;
  }

  auto uuid = UUID::from_string(uuid_json.value_unsafe());
  if (!uuid.has_value()) {
    return nullopt;
  }
  meta.uuid = uuid.value();

  if (auto hash_json = meta_json->doc["source_hash"].get_string(); !hash_json.error()) {
    meta.source_hash = string_to_hash(hash_json.value_unsafe());
  }

  if (auto textures_json = meta_json->doc["textures"].get_array(); !textures_json.error()) {
    for (auto texture_json : textures_json.value_unsafe()) {
      auto& texture = meta.textures.emplace_back();

      if (auto tex_uuid = texture_json["uuid"].get_string(); !tex_uuid.error()) {
        texture.uuid = UUID::from_string(tex_uuid.value_unsafe()).value_or(UUID(nullptr));
      }
      if (auto external = texture_json["external"].get_string(); !external.error()) {
        texture.external = std::filesystem::path(std::string(external.value_unsafe()));
      }
      if (auto cache = texture_json["cache"].get_string(); !cache.error()) {
        texture.cache = std::string(cache.value_unsafe());
      }
      if (auto srgb = texture_json["srgb"].get_bool(); !srgb.error()) {
        texture.is_srgb = srgb.value_unsafe();
      }
    }
  }

  if (auto materials_json = meta_json->doc["materials"].get_array(); !materials_json.error()) {
    for (auto material_json : materials_json.value_unsafe()) {
      auto material_uuid = UUID(nullptr);
      if (auto mat_uuid = material_json["uuid"].get_string(); !mat_uuid.error()) {
        material_uuid = UUID::from_string(mat_uuid.value_unsafe()).value_or(UUID(nullptr));
      }

      auto material = Material{};
      read_material_asset_meta(material_json.value_unsafe(), material);

      meta.material_uuids.push_back(material_uuid);
      meta.materials.push_back(material);
    }
  }

  return meta;
}

auto write_model_meta(const std::filesystem::path& source_path, const ImportedModelMeta& meta) -> bool {
  ZoneScoped;

  JsonWriter writer{};
  begin_asset_meta(writer, meta.uuid, AssetType::Model);
  writer["source_hash"] = hash_to_string(meta.source_hash);

  writer["textures"].begin_array();
  for (const auto& texture : meta.textures) {
    writer.begin_obj();
    writer["uuid"] = texture.uuid.str();
    writer["external"] = texture.external.generic_string();
    writer["cache"] = texture.cache;
    writer["srgb"] = texture.is_srgb;
    writer.end_obj();
  }
  writer.end_array();

  writer["materials"].begin_array();
  for (const auto& [material_uuid, material] : std::views::zip(meta.material_uuids, meta.materials)) {
    write_material_asset_meta(writer, material_uuid, material);
  }
  writer.end_array();

  return end_asset_meta(writer, source_path);
}

auto write_simple_meta(const std::filesystem::path& source_path, const UUID& uuid, AssetType type, u64 hash) -> bool {
  ZoneScoped;

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, type);
  writer["source_hash"] = hash_to_string(hash);

  return end_asset_meta(writer, source_path);
}

auto write_texture_meta(
  const std::filesystem::path& source_path, const UUID& uuid, u64 hash, option<bool> srgb, option<bool> directive
) -> bool {
  ZoneScoped;

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, AssetType::Texture);
  writer["source_hash"] = hash_to_string(hash);
  // Written only when it overrides the source, so a file that already declares its colour space
  // keeps tracking what it declares.
  if (srgb.has_value()) {
    writer["color_space"] = *srgb ? "srgb" : "linear";
  }
  // The last directive a model handed down, remembered so that an import with no opinion of its own
  // does not reset the file to what it declares. See `import_compiled_texture`.
  if (directive.has_value()) {
    writer["model_color_space"] = *directive ? "srgb" : "linear";
  }

  return end_asset_meta(writer, source_path);
}

// Recompiles the glTF and fills in every UUID the sidecar has to keep stable, reusing the ones
// already recorded so scenes that reference a material or texture survive a source edit.
auto compile_model(
  AssetManager& asset_man, rc::Session& session, const std::filesystem::path& path, ImportedModelMeta& meta
) -> bool {
  ZoneScoped;

  auto compiled = option<rc::ModelCompileResult>{nullopt};
  {
    auto log = SessionLog(session);
    compiled = session.process(rc::ModelCompileRequest{.path = path, .name = path.filename().string()});
  }

  if (!compiled.has_value()) {
    OX_LOG_ERROR("Failed to compile model '{}'.", path);
    return false;
  }

  auto& model = compiled->model;
  const auto model_dir = path.parent_path();

  auto previous_textures = std::move(meta.textures);
  meta.textures.clear();
  meta.textures.resize(model.textures.size());

  for (auto texture_index = 0_sz; texture_index < model.textures.size(); texture_index++) {
    auto& entry = meta.textures[texture_index];
    auto& compiled_texture = compiled->textures[texture_index];

    entry.is_srgb = model.textures[texture_index].is_srgb;

    if (compiled_texture.kind == rc::CompiledTexture::Kind::External) {
      // an asset in its own right, so its own sidecar owns the UUID -- but the model still oversees
      // how its own resources are read, so the slot's colour space goes down with it
      entry.external = compiled_texture.external_path;
      entry.uuid = import_asset(asset_man, session, model_dir / compiled_texture.external_path, entry.is_srgb);
      model.textures[texture_index].uuid = PackedUUID::pack(entry.uuid);
      continue;
    }

    if (compiled_texture.kind == rc::CompiledTexture::Kind::None) {
      continue;
    }

    entry.uuid = texture_index < previous_textures.size() && previous_textures[texture_index].uuid
                   ? previous_textures[texture_index].uuid
                   : UUID::generate_random();

    entry.cache = entry.uuid.str() + ".oxpack";
    if (!write_texture_pack(cache_dir() / entry.cache, std::move(compiled_texture.data), entry.uuid)) {
      return false;
    }

    model.textures[texture_index].uuid = PackedUUID::pack(entry.uuid);
  }

  auto texture_uuids = std::vector<UUID>();
  texture_uuids.reserve(meta.textures.size());
  for (const auto& texture : meta.textures) {
    texture_uuids.push_back(texture.uuid);
  }

  auto previous_material_uuids = std::move(meta.material_uuids);
  meta.material_uuids.clear();
  meta.materials.clear();
  for (auto material_index = 0_sz; material_index < model.materials.size(); material_index++) {
    const auto material_uuid = material_index < previous_material_uuids.size() &&
                                   previous_material_uuids[material_index]
                                 ? previous_material_uuids[material_index]
                                 : UUID::generate_random();

    model.materials[material_index].uuid = PackedUUID::pack(material_uuid);
    meta.material_uuids.push_back(material_uuid);
    meta.materials.push_back(to_material(model.materials[material_index], texture_uuids));
  }

  auto file = AssetFile{};
  file.add_entry(std::move(model), PackedUUID::pack(meta.uuid));
  if (!file.pack(cache_path(meta.uuid))) {
    OX_LOG_ERROR("Couldn't write the model cache for '{}'.", path);
    return false;
  }

  return true;
}

auto register_model(
  AssetManager& asset_man, rc::Session& session, const std::filesystem::path& path, const ImportedModelMeta& meta
) -> void {
  ZoneScoped;

  asset_man.register_asset(meta.uuid, AssetType::Model, cache_path(meta.uuid));

  const auto model_dir = path.parent_path();
  for (const auto& texture : meta.textures) {
    if (!texture.uuid) {
      continue;
    }

    if (!texture.external.empty()) {
      // the sibling file's own sidecar owns this UUID, but the directive has to be repeated: it is
      // mixed into the texture's staleness hash, so dropping it here recompiles the pack against
      // the colour space the file declares and undoes what the compile above resolved
      import_asset(asset_man, session, model_dir / texture.external, texture.is_srgb);
      continue;
    }

    asset_man.register_asset(texture.uuid, AssetType::Texture, cache_dir() / texture.cache);
  }

  for (const auto& [material_uuid, material] : std::views::zip(meta.material_uuids, meta.materials)) {
    if (!material_uuid) {
      continue;
    }

    // The model's own load hands these over too, but a scene can name a material long before the
    // model it came from is touched, and then this is the only source.
    asset_man.register_asset(material_uuid, AssetType::Material, path);
    asset_man.set_pending_load_info(material_uuid, material);
  }
}

auto import_model(AssetManager& asset_man, rc::Session& session, const std::filesystem::path& path) -> UUID {
  ZoneScoped;

  const auto meta_path = meta_file_path(path);
  auto meta = std::filesystem::exists(meta_path) ? read_model_meta(meta_path) : nullopt;
  if (!meta.has_value()) {
    meta = ImportedModelMeta{.uuid = UUID::generate_random()};
  }

  const auto hash = source_hash(path);
  const auto stale = meta->source_hash != hash || !std::filesystem::exists(cache_path(meta->uuid));
  if (stale) {
    meta->source_hash = hash;
    if (!compile_model(asset_man, session, path, meta.value())) {
      return UUID(nullptr);
    }

    if (!write_model_meta(path, meta.value())) {
      return UUID(nullptr);
    }
  }

  register_model(asset_man, session, path, meta.value());

  return meta->uuid;
}

auto import_compiled_texture(
  AssetManager& asset_man, rc::Session& session, const std::filesystem::path& path, option<bool> srgb_directive
) -> UUID {
  ZoneScoped;

  const auto meta_path = meta_file_path(path);
  auto uuid = UUID(nullptr);
  auto recorded_hash = 0_u64;
  // KTX2 and DDS both declare their own colour space, but a model that reaches this file knows what
  // it is actually for, which is better evidence than a label an exporter guessed at. The sidecar
  // carries a flag when the user overrides both, and editing it there forces a recompile.
  auto srgb = option<bool>(nullopt);
  auto recorded_directive = option<bool>(nullopt);
  if (auto meta_json = std::filesystem::exists(meta_path) ? read_meta_file(meta_path) : nullptr) {
    if (auto uuid_json = meta_json->doc["uuid"].get_string(); !uuid_json.error()) {
      uuid = UUID::from_string(uuid_json.value_unsafe()).value_or(UUID(nullptr));
    }
    if (auto hash_json = meta_json->doc["source_hash"].get_string(); !hash_json.error()) {
      recorded_hash = string_to_hash(hash_json.value_unsafe());
    }
    if (auto space_json = meta_json->doc["color_space"].get_string(); !space_json.error()) {
      srgb = space_json.value_unsafe() == "srgb";
    }
    if (auto directive_json = meta_json->doc["model_color_space"].get_string(); !directive_json.error()) {
      recorded_directive = directive_json.value_unsafe() == "srgb";
    }
  }

  if (!uuid) {
    uuid = UUID::generate_random();
  }

  // A caller with no opinion inherits the last directive rather than falling back to the source, so
  // the order a project scan happens to walk the directory in cannot flip an already-cooked pack.
  // A model that names this file still passes its own directive on every import, so moving a texture
  // to another material slot takes effect immediately.
  const auto directive = srgb_directive.has_value() ? srgb_directive : recorded_directive;
  const auto resolved = srgb.has_value() ? srgb : directive;
  const auto hash = mix_hash(source_hash(path), resolved.has_value() ? (*resolved ? 1 : 2) : 0);
  const auto pack_path = cache_path(uuid);
  if (recorded_hash != hash || !std::filesystem::exists(pack_path)) {
    auto data = option<TextureData>{nullopt};
    {
      auto log = SessionLog(session);
      data = session.process(
        rc::TextureCompileRequest{.path = path, .name = path.filename().string(), .srgb = resolved}
      );
    }

    if (!data.has_value()) {
      OX_LOG_ERROR("Failed to compile texture '{}'.", path);
      return UUID(nullptr);
    }

    if (!write_texture_pack(pack_path, std::move(data.value()), uuid)) {
      OX_LOG_ERROR("Failed to write the texture pack for '{}'.", path);
      return UUID(nullptr);
    }

    if (!write_texture_meta(path, uuid, hash, srgb, directive)) {
      return UUID(nullptr);
    }
  }

  asset_man.register_asset(uuid, AssetType::Texture, pack_path);

  return uuid;
}

auto import_asset(
  AssetManager& asset_man, rc::Session& session, const std::filesystem::path& path, option<bool> srgb_directive
) -> UUID {
  ZoneScoped;

  if (!std::filesystem::exists(path)) {
    OX_LOG_ERROR("Trying to import an asset '{}' that doesn't exist.", path);
    return UUID(nullptr);
  }

  const auto claim = ImportClaim(path);

  const auto file_type = to_asset_file_type(path);
  if (file_type == AssetFileType::Meta) {
    // A sidecar next to a file the importer understands is not the asset -- the source is, and only
    // the source drives the staleness check. Otherwise the sidecar is the whole asset.
    auto source_path = path;
    source_path.replace_extension("");
    if (std::filesystem::exists(source_path) && to_asset_type(to_asset_file_type(source_path)) != AssetType::None) {
      return import_asset(asset_man, session, source_path, srgb_directive);
    }

    return register_asset_from_meta(asset_man, path);
  }

  const auto asset_type = to_asset_type(file_type);
  if (asset_type == AssetType::None) {
    return UUID(nullptr);
  }

  if (needs_compiling(file_type)) {
    auto error = std::error_code{};
    std::filesystem::create_directories(cache_dir(), error);
    if (error) {
      OX_LOG_ERROR("Couldn't create the asset cache directory {}: {}", cache_dir(), error.message());
      return UUID(nullptr);
    }

    if (asset_type == AssetType::Model) {
      return import_model(asset_man, session, path);
    }

    return import_compiled_texture(asset_man, session, path, srgb_directive);
  }

  // Everything else the engine reads straight from its source file.
  const auto meta_path = meta_file_path(path);
  if (std::filesystem::exists(meta_path)) {
    return register_asset_from_meta(asset_man, meta_path);
  }

  const auto uuid = asset_man.create_asset(asset_type, path);
  if (!uuid) {
    return UUID(nullptr);
  }

  if (!write_simple_meta(path, uuid, asset_type, source_hash(path))) {
    return UUID(nullptr);
  }

  return uuid;
}

auto import_asset(AssetManager& asset_man, const std::filesystem::path& path, option<bool> srgb_directive) -> UUID {
  ZoneScoped;

  return import_asset(asset_man, App::mod<rc::ResourceCompiler>(), path, srgb_directive);
}
} // namespace ox
