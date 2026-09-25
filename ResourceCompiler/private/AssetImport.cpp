#include "AssetImport.hpp"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <fmt/format.h>
#include <fmt/std.h>
#include <memory>
#include <mutex>
#include <ranges>
#include <simdjson.h>
#include <span>
#include <utility>

#include "Asset/AssetManifest.hpp"
#include "Core/VFS.hpp"
#include "Memory/Hasher.hpp"
#include "Memory/Stack.hpp"
#include "OS/File.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox::rc {
// `doc` borrows `contents` and `parser`, so the declaration order is the destruction order
struct MetaFile {
  simdjson::padded_string contents;
  simdjson::ondemand::parser parser;
  simdjson::simdjson_result<simdjson::ondemand::document> doc;
};

// A parallel scan can reach one file from several threads at once: two models naming the same sibling texture, or a
// source and its own `.oxasset` sitting side by side in one directory. Whoever claims a path first does the work and
// the others wait, so nothing is compiled twice or handed two UUIDs. The waiter then takes the warm path, which is
// why no result needs caching here.
//
// Claims nest (a model claims itself, then its textures) but never cycle: a texture import cannot reach a model.
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

// what one `import_asset` call threads through its recursion
struct Importer {
  Session& session;
  std::filesystem::path cooked_dir = {};
  ImportResult& result;
};

// What the sidecar records for one of a model's textures. Exactly one of the two paths is set: `external` for a
// sibling file that is an asset in its own right, `cache` for one the compiler produced and only this model refers
// to. `is_srgb` is the colour space the glTF's material graph asked for, kept because a re-registration has to
// repeat the directive the compile made.
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

static auto import_asset(Importer& importer, const std::filesystem::path& path, option<bool> srgb_directive) -> UUID;

static auto header_matches(std::span<const u8> header, std::span<const u8> magic, const usize offset = 0) -> bool {
  return header.size() >= offset + magic.size() && std::memcmp(header.data() + offset, magic.data(), magic.size()) == 0;
}

// What the file says it is, which a rename cannot change. Only formats that carry a signature are here: glTF, Lua,
// JSON and the sidecars are text, and the two `ox` formats are ours to name.
static auto to_asset_file_signature(const std::filesystem::path& path) -> AssetFileType {
  ZoneScoped;

  auto error = std::error_code{};
  if (!std::filesystem::is_regular_file(path, error)) {
    return AssetFileType::None;
  }

  auto file = File(path, FileAccess::Read);
  if (!file) {
    return AssetFileType::None;
  }

  // as much as the longest signature checked below, KTX2's twelve bytes, plus the tag WAV carries past its RIFF
  // header
  auto bytes = std::array<u8, 16>{};
  const auto read = file.read(bytes.data(), bytes.size());
  const auto header = std::span(bytes).first(ox::min(static_cast<usize>(read), bytes.size()));

  constexpr u8 PNG[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  constexpr u8 JPEG[] = {0xFF, 0xD8, 0xFF};
  constexpr u8 KTX2[] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  constexpr u8 DDS[] = {'D', 'D', 'S', ' '};
  constexpr u8 GLB[] = {'g', 'l', 'T', 'F'};
  constexpr u8 OGG[] = {'O', 'g', 'g', 'S'};
  constexpr u8 FLAC[] = {'f', 'L', 'a', 'C'};
  constexpr u8 RIFF[] = {'R', 'I', 'F', 'F'};
  constexpr u8 WAVE[] = {'W', 'A', 'V', 'E'};
  constexpr u8 ID3[] = {'I', 'D', '3'};

  if (header_matches(header, PNG)) {
    return AssetFileType::PNG;
  }
  if (header_matches(header, JPEG)) {
    return AssetFileType::JPEG;
  }
  if (header_matches(header, KTX2)) {
    return AssetFileType::KTX2;
  }
  if (header_matches(header, DDS)) {
    return AssetFileType::DDS;
  }
  if (header_matches(header, GLB)) {
    return AssetFileType::GLB;
  }
  if (header_matches(header, OGG)) {
    return AssetFileType::OGG;
  }
  if (header_matches(header, FLAC)) {
    return AssetFileType::FLAC;
  }
  // RIFF is a container, so the form it holds is what decides
  if (header_matches(header, RIFF) && header_matches(header, WAVE, 8)) {
    return AssetFileType::WAV;
  }
  // MP3 is the one format with no real magic: an ID3 tag if it has one, otherwise the eleven set bits an mp3 frame
  // starts with. Checked last because that sync pattern is weak evidence next to everything above.
  if (header_matches(header, ID3) || (header.size() >= 2 && header[0] == 0xFF && (header[1] & 0xE0) == 0xE0)) {
    return AssetFileType::MP3;
  }

  return AssetFileType::None;
}

auto to_asset_file_type(const std::filesystem::path& path) -> AssetFileType {
  ZoneScoped;
  memory::ScopedStack stack;

  // The signature is the stronger evidence, so it settles the question whenever the file has one: a jpeg saved as
  // `.png` reaches a decoder that cannot read it otherwise. The extension answers for text formats, and for a path
  // that is not on disk yet -- a sidecar named for a file that is about to be written.
  if (const auto signature = to_asset_file_signature(path); signature != AssetFileType::None) {
    return signature;
  }

  if (!path.has_extension()) {
    return AssetFileType::None;
  }

  auto extension = stack.to_upper(path.extension().string());
  switch (fnv64_str(extension)) {
    case fnv64_c(".GLB")       : return AssetFileType::GLB;
    case fnv64_c(".GLTF")      : return AssetFileType::GLTF;
    case fnv64_c(".PNG")       : return AssetFileType::PNG;
    case fnv64_c(".JPG")       :
    case fnv64_c(".JPEG")      : return AssetFileType::JPEG;
    case fnv64_c(".DDS")       : return AssetFileType::DDS;
    case fnv64_c(".JSON")      : return AssetFileType::JSON;
    case fnv64_c(".OXASSET")   : return AssetFileType::Meta;
    case fnv64_c(".KTX2")      : return AssetFileType::KTX2;
    case fnv64_c(".LUA")       : return AssetFileType::LUA;
    case fnv64_c(".OXTERRAIN") : return AssetFileType::OXTERRAIN;
    case fnv64_c(".OXPARTICLE"): return AssetFileType::OXPARTICLE;
    case fnv64_c(".WAV")       : return AssetFileType::WAV;
    case fnv64_c(".MP3")       : return AssetFileType::MP3;
    case fnv64_c(".FLAC")      : return AssetFileType::FLAC;
    case fnv64_c(".OGG")       : return AssetFileType::OGG;
    default                    : return AssetFileType::None;
  }
}

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
    case AssetFileType::WAV       :
    case AssetFileType::MP3       :
    case AssetFileType::FLAC      :
    case AssetFileType::OGG       : return AssetType::Audio;
    default                       : return AssetType::None;
  }
}

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

auto meta_file_path(const std::filesystem::path& path) -> std::filesystem::path {
  ZoneScoped;

  if (path.empty()) {
    return {};
  }

  if (to_asset_file_type(path) == AssetFileType::Meta) {
    return path;
  }

  auto meta_path = path;
  meta_path += ".oxasset";

  return meta_path;
}

auto owns_meta_file(const std::filesystem::path& path) -> bool {
  ZoneScoped;

  if (path.empty()) {
    return false;
  }

  const auto file_type = to_asset_file_type(path);

  return file_type == AssetFileType::None || file_type == AssetFileType::Meta;
}

auto source_hash(const std::filesystem::path& path) -> u64 {
  ZoneScoped;

  auto file = File(path, FileAccess::Read);
  if (!file) {
    return 0;
  }

  auto hash = file.size == 0 ? 0_u64 : ankerl::unordered_dense::detail::wyhash::hash(file.map(), file.size);
  hash = ankerl::unordered_dense::detail::wyhash::mix(hash, ASSET_COMPILER_VERSION);
  hash = ankerl::unordered_dense::detail::wyhash::mix(hash, AssetFileHeader::VERSION);

  return hash;
}

auto begin_asset_meta(JsonWriter& writer, const UUID& uuid, AssetType type) -> void {
  ZoneScoped;

  writer.begin_obj();
  writer["uuid"] = uuid.str();
  writer["type"] = std::to_underlying(type);
}

auto end_asset_meta(JsonWriter& writer, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  writer.end_obj();

  // a rewrite that changes nothing would still bump the mtime, and a build that touches sources rebuilds forever
  const auto meta_path = meta_file_path(path);
  const auto contents = writer.stream.view();
  if (std::filesystem::exists(meta_path) && File::to_string(meta_path) == contents) {
    return true;
  }

  auto file = File(meta_path, FileAccess::Write);
  if (!file) {
    return false;
  }

  file.write(contents);
  file.close();
  return true;
}

auto write_material_asset_meta(JsonWriter& writer, const UUID& uuid, const Material& material) -> bool {
  ZoneScoped;

  writer.begin_obj();

  writer["uuid"] = uuid.str();
  writer["sampling_mode"] = static_cast<u32>(material.sampling_mode);
  writer["albedo_color"] = material.albedo_color;
  writer["uv_size"] = material.uv_size;
  writer["uv_offset"] = material.uv_offset;
  writer["emissive_color"] = material.emissive_color;
  writer["roughness_factor"] = material.roughness_factor;
  writer["metallic_factor"] = material.metallic_factor;
  writer["normal_scale"] = material.normal_scale;
  writer["occlusion_strength"] = material.occlusion_strength;
  writer["alpha_mode"] = std::to_underlying(material.alpha_mode);
  writer["alpha_cutoff"] = material.alpha_cutoff;
  writer["flip_normal_y"] = material.flip_normal_y;
  writer["albedo_texture"] = material.albedo_texture.str().c_str();
  writer["normal_texture"] = material.normal_texture.str().c_str();
  writer["emissive_texture"] = material.emissive_texture.str().c_str();
  writer["metallic_roughness_texture"] = material.metallic_roughness_texture.str().c_str();
  writer["occlusion_texture"] = material.occlusion_texture.str().c_str();

  writer.end_obj();

  return true;
}

static auto read_material_asset_meta(simdjson::ondemand::value json, Material& material) -> void {
  ZoneScoped;

  const auto read_f32 = [&json](std::string_view key, f32& value) {
    auto result = json[key].get_double();
    if (!result.error()) {
      value = static_cast<f32>(result.value_unsafe());
    }
  };

  const auto read_enum = [&json]<typename T>(std::string_view key, T& value) {
    auto result = json[key].get_uint64();
    if (!result.error()) {
      value = static_cast<T>(result.value_unsafe());
    }
  };

  const auto read_bool = [&json](std::string_view key, bool& value) {
    auto result = json[key].get_bool();
    if (!result.error()) {
      value = result.value_unsafe();
    }
  };

  const auto read_uuid = [&json](std::string_view key, UUID& value) {
    auto result = json[key].get_string();
    if (result.error()) {
      return;
    }

    if (auto uuid = UUID::from_string(result.value_unsafe()); uuid.has_value()) {
      value = uuid.value();
    }
  };

  const auto read_vec = [&json]<glm::length_t N>(std::string_view key, glm::vec<N, f32>& value) {
    constexpr static std::string_view components[] = {"x", "y", "z", "w"};
    auto field = json[key];
    if (field.error()) {
      return;
    }

    for (glm::length_t i = 0; i < N; i++) {
      auto result = field[components[i]].get_double();
      if (!result.error()) {
        value[i] = static_cast<f32>(result.value_unsafe());
      }
    }
  };

  read_enum("sampling_mode", material.sampling_mode);
  read_enum("alpha_mode", material.alpha_mode);
  read_vec("albedo_color", material.albedo_color);
  read_vec("uv_size", material.uv_size);
  read_vec("uv_offset", material.uv_offset);
  read_vec("emissive_color", material.emissive_color);
  read_f32("roughness_factor", material.roughness_factor);
  read_f32("metallic_factor", material.metallic_factor);
  read_f32("normal_scale", material.normal_scale);
  read_f32("occlusion_strength", material.occlusion_strength);
  read_f32("alpha_cutoff", material.alpha_cutoff);
  read_bool("flip_normal_y", material.flip_normal_y);
  read_uuid("albedo_texture", material.albedo_texture);
  read_uuid("normal_texture", material.normal_texture);
  read_uuid("emissive_texture", material.emissive_texture);
  read_uuid("metallic_roughness_texture", material.metallic_roughness_texture);
  read_uuid("occlusion_texture", material.occlusion_texture);
}

static auto read_meta_file(Session& session, const std::filesystem::path& path) -> std::unique_ptr<MetaFile> {
  ZoneScoped;

  auto content = File::to_string(path);
  if (content.empty()) {
    session.push_error(fmt::format("Failed to read {}.", path));
    return nullptr;
  }

  auto meta_file = std::make_unique<MetaFile>();
  meta_file->contents = simdjson::padded_string(content);
  meta_file->doc = meta_file->parser.iterate(meta_file->contents);
  if (meta_file->doc.error()) {
    session.push_error(fmt::format("Failed to parse {}: {}", path, simdjson::error_message(meta_file->doc.error())));
    return nullptr;
  }

  return meta_file;
}

static auto hash_to_string(u64 hash) -> std::string { return fmt::format("{:016X}", hash); }

static auto string_to_hash(std::string_view text) -> u64 {
  auto value = 0_u64;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc{}) {
    return 0;
  }

  return value;
}

static auto mix_hash(u64 hash, u64 value) -> u64 { return (hash ^ value) * 0x100000001B3_u64; }

static auto cooked_path(const Importer& importer, const UUID& uuid) -> std::filesystem::path {
  return importer.cooked_dir / (uuid.str() + ".oxpack");
}

// a build that has to create one is writing into the source tree, so say so: the new UUID only stays put if the
// sidecar is committed
static auto note_new_sidecar(Importer& importer, const std::filesystem::path& path) -> void {
  importer.session.push_message(fmt::format("Created {}, commit it so its UUID stays stable.", meta_file_path(path)));
}

static auto write_texture_pack(const std::filesystem::path& path, TextureData&& data, const UUID& uuid) -> bool {
  auto file = AssetFile{};
  file.add_entry(std::move(data), PackedUUID::pack(uuid));

  return file.pack(path);
}

static auto read_model_meta(Session& session, const std::filesystem::path& meta_path) -> option<ImportedModelMeta> {
  ZoneScoped;

  auto meta_json = read_meta_file(session, meta_path);
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

static auto write_model_meta(const std::filesystem::path& source_path, const ImportedModelMeta& meta) -> bool {
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

static auto write_simple_meta(const std::filesystem::path& source_path, const UUID& uuid, AssetType type, u64 hash)
  -> bool {
  ZoneScoped;

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, type);
  writer["source_hash"] = hash_to_string(hash);

  return end_asset_meta(writer, source_path);
}

static auto write_texture_meta(
  const std::filesystem::path& source_path, const UUID& uuid, u64 hash, option<bool> srgb, option<bool> directive
) -> bool {
  ZoneScoped;

  JsonWriter writer{};
  begin_asset_meta(writer, uuid, AssetType::Texture);
  writer["source_hash"] = hash_to_string(hash);
  // Written only when it overrides the source, so a file that already declares its colour space keeps tracking what
  // it declares.
  if (srgb.has_value()) {
    writer["color_space"] = *srgb ? "srgb" : "linear";
  }
  // The last directive a model handed down, remembered so that an import with no opinion of its own does not reset
  // the file to what it declares. See `import_compiled_texture`.
  if (directive.has_value()) {
    writer["model_color_space"] = *directive ? "srgb" : "linear";
  }

  return end_asset_meta(writer, source_path);
}

// Recompiles the glTF and fills in every UUID the sidecar has to keep stable, reusing the ones already recorded so
// scenes that reference a material or texture survive a source edit.
static auto compile_model(Importer& importer, const std::filesystem::path& path, ImportedModelMeta& meta) -> bool {
  ZoneScoped;

  auto compiled = importer.session.process(ModelCompileRequest{.path = path, .name = path.filename().string()});
  if (!compiled.has_value()) {
    importer.session.push_error(fmt::format("Failed to compile model '{}'.", path));
    return false;
  }

  auto& model = compiled->model;
  const auto model_dir = path.parent_path();

  auto previous_textures = std::move(meta.textures);
  meta.textures.clear();
  meta.textures.resize(model.textures.size());

  // Two slots can name the same sibling file with different colour spaces -- a glTF that uses one image as both a
  // base colour and a normal map. There is one pack per file, so the first slot to claim it settles the question for
  // the rest; letting each slot pass its own directive down leaves them fighting over one cache entry, and the file
  // recompiles on every import forever.
  auto external_srgb = ankerl::unordered_dense::map<std::string, bool>{};

  for (auto texture_index = 0_sz; texture_index < model.textures.size(); texture_index++) {
    auto& entry = meta.textures[texture_index];
    auto& compiled_texture = compiled->textures[texture_index];

    entry.is_srgb = model.textures[texture_index].is_srgb;

    if (compiled_texture.kind == CompiledTexture::Kind::External) {
      // an asset in its own right, so its own sidecar owns the UUID -- but the model still oversees how its own
      // resources are read, so the slot's colour space goes down with it
      const auto texture_path = (model_dir / compiled_texture.external_path).lexically_normal();
      const auto [claim, claimed] = external_srgb.try_emplace(texture_path.string(), entry.is_srgb);
      if (!claimed && claim->second != entry.is_srgb) {
        importer.session.push_message(
          fmt::format(
            "'{}' is used as both an sRGB and a linear texture by '{}'. Cooking it as {}.",
            texture_path,
            path,
            claim->second ? "sRGB" : "linear"
          )
        );
      }

      entry.is_srgb = claim->second;
      // what the pack ends up holding, so the engine's load-time colour space check agrees with it
      model.textures[texture_index].is_srgb = entry.is_srgb;
      entry.external = compiled_texture.external_path;
      entry.uuid = import_asset(importer, texture_path, entry.is_srgb);
      model.textures[texture_index].uuid = PackedUUID::pack(entry.uuid);
      continue;
    }

    if (compiled_texture.kind == CompiledTexture::Kind::None) {
      continue;
    }

    entry.uuid = texture_index < previous_textures.size() && previous_textures[texture_index].uuid
                   ? previous_textures[texture_index].uuid
                   : UUID::generate_random();

    entry.cache = entry.uuid.str() + ".oxpack";
    if (!write_texture_pack(importer.cooked_dir / entry.cache, std::move(compiled_texture.data), entry.uuid)) {
      importer.session.push_error(fmt::format("Couldn't write a texture pack for '{}'.", path));
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
  if (!file.pack(cooked_path(importer, meta.uuid))) {
    importer.session.push_error(fmt::format("Couldn't write the model pack for '{}'.", path));
    return false;
  }

  return true;
}

// The model and everything it brings along. A sibling texture registered itself when `compile_model` imported it,
// but a warm model skips the compile, so it goes through the importer again here.
static auto add_model(Importer& importer, const std::filesystem::path& path, const ImportedModelMeta& meta) -> void {
  ZoneScoped;

  const auto source_name = path.filename().string();
  importer.result.assets.push_back(
    ImportedAsset{
      .uuid = meta.uuid,
      .type = AssetType::Model,
      .path = cooked_path(importer, meta.uuid),
      .source_path = path,
      .origin = path,
      .name = source_name,
    }
  );

  const auto model_dir = path.parent_path();
  for (const auto& [texture_index, texture] : std::views::enumerate(meta.textures)) {
    if (!texture.uuid) {
      continue;
    }

    if (!texture.external.empty()) {
      // the sibling file's own sidecar owns this UUID, but the directive has to be repeated: it is mixed into the
      // texture's staleness hash, so dropping it here recompiles the pack against the colour space the file declares
      // and undoes what the compile above resolved
      import_asset(importer, model_dir / texture.external, texture.is_srgb);
      continue;
    }

    // an image that lives inside the glTF has no file of its own, so the slot it fills is all there is to tell it
    // apart from its siblings in a list
    importer.result.assets.push_back(
      ImportedAsset{
        .uuid = texture.uuid,
        .type = AssetType::Texture,
        .path = importer.cooked_dir / texture.cache,
        .origin = path,
        .name = fmt::format("{} (texture {})", source_name, texture_index),
      }
    );
  }

  for (auto material_index = 0_sz; material_index < meta.material_uuids.size(); material_index++) {
    const auto& material_uuid = meta.material_uuids[material_index];
    if (!material_uuid) {
      continue;
    }

    // The model's own load hands these over too, but a scene can name a material long before the model it came from
    // is touched, and then this is the only source.
    importer.result.assets.push_back(
      ImportedAsset{
        .uuid = material_uuid,
        .type = AssetType::Material,
        .path = path,
        .origin = path,
        .name = fmt::format("{} (material {})", source_name, material_index),
        .material = meta.materials[material_index],
      }
    );
  }
}

static auto import_model(Importer& importer, const std::filesystem::path& path) -> UUID {
  ZoneScoped;

  const auto meta_path = meta_file_path(path);
  const auto had_meta = std::filesystem::exists(meta_path);
  auto meta = had_meta ? read_model_meta(importer.session, meta_path) : nullopt;
  if (!meta.has_value()) {
    meta = ImportedModelMeta{.uuid = UUID::generate_random()};
  }

  const auto hash = source_hash(path);
  const auto stale = meta->source_hash != hash || !std::filesystem::exists(cooked_path(importer, meta->uuid));
  if (stale) {
    meta->source_hash = hash;
    if (!compile_model(importer, path, meta.value())) {
      return UUID(nullptr);
    }

    if (!write_model_meta(path, meta.value())) {
      importer.session.push_error(fmt::format("Couldn't write {}.", meta_path));
      return UUID(nullptr);
    }

    if (!had_meta) {
      note_new_sidecar(importer, path);
    }
  }

  add_model(importer, path, meta.value());

  return meta->uuid;
}

static auto import_compiled_texture(Importer& importer, const std::filesystem::path& path, option<bool> srgb_directive)
  -> UUID {
  ZoneScoped;

  const auto meta_path = meta_file_path(path);
  const auto had_meta = std::filesystem::exists(meta_path);
  auto uuid = UUID(nullptr);
  auto recorded_hash = 0_u64;
  // KTX2 and DDS both declare their own colour space, but a model that reaches this file knows what it is actually
  // for, which is better evidence than a label an exporter guessed at. The sidecar carries a flag when the user
  // overrides both, and editing it there forces a recompile.
  auto srgb = option<bool>(nullopt);
  auto recorded_directive = option<bool>(nullopt);
  if (auto meta_json = had_meta ? read_meta_file(importer.session, meta_path) : nullptr) {
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

  // A caller with no opinion inherits the last directive rather than falling back to the source, so the order a
  // project scan happens to walk the directory in cannot flip an already-cooked pack. A model that names this file
  // still passes its own directive on every import, so moving a texture to another material slot takes effect
  // immediately.
  const auto directive = srgb_directive.has_value() ? srgb_directive : recorded_directive;
  const auto resolved = srgb.has_value() ? srgb : directive;
  const auto hash = mix_hash(source_hash(path), resolved.has_value() ? (*resolved ? 1 : 2) : 0);
  const auto pack_path = cooked_path(importer, uuid);
  if (recorded_hash != hash || !std::filesystem::exists(pack_path)) {
    auto data = importer.session.process(
      TextureCompileRequest{.path = path, .name = path.filename().string(), .srgb = resolved}
    );
    if (!data.has_value()) {
      importer.session.push_error(fmt::format("Failed to compile texture '{}'.", path));
      return UUID(nullptr);
    }

    if (!write_texture_pack(pack_path, std::move(data.value()), uuid)) {
      importer.session.push_error(fmt::format("Failed to write the texture pack for '{}'.", path));
      return UUID(nullptr);
    }

    if (!write_texture_meta(path, uuid, hash, srgb, directive)) {
      importer.session.push_error(fmt::format("Couldn't write {}.", meta_path));
      return UUID(nullptr);
    }

    if (!had_meta) {
      note_new_sidecar(importer, path);
    }
  }

  importer.result.assets.push_back(
    ImportedAsset{
      .uuid = uuid,
      .type = AssetType::Texture,
      .path = pack_path,
      .source_path = path,
      .origin = path,
      .name = path.filename().string(),
    }
  );

  return uuid;
}

// Everything that has no payload of its own to cook: a sidecar standing alone is the whole asset (a material), and a
// source the engine reads directly (scripts, audio) is registered against itself.
static auto import_from_meta(Importer& importer, const std::filesystem::path& meta_path) -> UUID {
  ZoneScoped;

  auto meta_json = read_meta_file(importer.session, meta_path);
  if (!meta_json) {
    return UUID(nullptr);
  }

  // single pass, so these have to be read in the order `begin_asset_meta` wrote them: uuid, then type, then the
  // per-type payload
  auto uuid_json = meta_json->doc["uuid"].get_string();
  if (uuid_json.error()) {
    importer.session.push_error(fmt::format("{} has no `uuid`.", meta_path));
    return UUID(nullptr);
  }

  auto type_json = meta_json->doc["type"].get_number();
  if (type_json.error()) {
    importer.session.push_error(fmt::format("{} has no `type`.", meta_path));
    return UUID(nullptr);
  }

  auto asset_path = meta_path;
  asset_path.replace_extension("");
  const auto uuid = UUID::from_string(uuid_json.value_unsafe()).value_or(UUID(nullptr));
  if (!uuid) {
    importer.session.push_error(fmt::format("{} has a malformed `uuid`.", meta_path));
    return UUID(nullptr);
  }

  auto asset = ImportedAsset{
    .uuid = uuid,
    .type = static_cast<AssetType>(type_json.value_unsafe().get_uint64()),
    .path = asset_path,
    .source_path = asset_path,
    .origin = asset_path,
    .name = asset_path.filename().string(),
  };

  // A material has no payload file: the sidecar is the whole asset.
  if (asset.type == AssetType::Material) {
    auto material = Material{};
    if (auto material_json = meta_json->doc["material"]; !material_json.error()) {
      read_material_asset_meta(material_json.value_unsafe(), material);
    }

    asset.material = material;
  }

  importer.result.assets.push_back(std::move(asset));

  return uuid;
}

static auto import_asset(Importer& importer, const std::filesystem::path& path, option<bool> srgb_directive) -> UUID {
  ZoneScoped;

  if (!std::filesystem::exists(path)) {
    importer.session.push_error(fmt::format("Trying to import '{}', which doesn't exist.", path));
    return UUID(nullptr);
  }

  const auto claim = ImportClaim(path);

  const auto file_type = to_asset_file_type(path);
  if (file_type == AssetFileType::Meta) {
    // A sidecar next to a file the importer understands is not the asset -- the source is, and only the source drives
    // the staleness check. Otherwise the sidecar is the whole asset.
    auto source_path = path;
    source_path.replace_extension("");
    if (std::filesystem::exists(source_path) && to_asset_type(to_asset_file_type(source_path)) != AssetType::None) {
      return import_asset(importer, source_path, srgb_directive);
    }

    return import_from_meta(importer, path);
  }

  const auto asset_type = to_asset_type(file_type);
  if (asset_type == AssetType::None) {
    return UUID(nullptr);
  }

  if (needs_compiling(file_type)) {
    auto error = std::error_code{};
    std::filesystem::create_directories(importer.cooked_dir, error);
    if (error) {
      importer.session.push_error(fmt::format("Couldn't create {}: {}", importer.cooked_dir, error.message()));
      return UUID(nullptr);
    }

    if (asset_type == AssetType::Model) {
      return import_model(importer, path);
    }

    return import_compiled_texture(importer, path, srgb_directive);
  }

  // Everything else the engine reads straight from its source file.
  const auto meta_path = meta_file_path(path);
  if (std::filesystem::exists(meta_path)) {
    return import_from_meta(importer, meta_path);
  }

  const auto uuid = UUID::generate_random();
  if (!write_simple_meta(path, uuid, asset_type, source_hash(path))) {
    importer.session.push_error(fmt::format("Couldn't write {}.", meta_path));
    return UUID(nullptr);
  }
  note_new_sidecar(importer, path);

  importer.result.assets.push_back(
    ImportedAsset{
      .uuid = uuid,
      .type = asset_type,
      .path = path,
      .source_path = path,
      .origin = path,
      .name = path.filename().string(),
    }
  );

  return uuid;
}

auto import_asset(
  Session& session,
  const std::filesystem::path& cooked_dir,
  const std::filesystem::path& path,
  option<bool> srgb_directive
) -> ImportResult {
  ZoneScoped;

  auto result = ImportResult{};
  auto importer = Importer{.session = session, .cooked_dir = cooked_dir, .result = result};
  result.uuid = import_asset(importer, path, srgb_directive);

  return result;
}

auto cook_assets(Session& session, const std::filesystem::path& assets_dir, const std::filesystem::path& output_dir)
  -> bool {
  ZoneScoped;

  auto error = std::error_code{};
  std::filesystem::create_directories(output_dir, error);
  if (error) {
    session.push_error(fmt::format("Couldn't create {}: {}", output_dir, error.message()));
    return false;
  }

  // sorted, so two cooks of the same tree produce the same manifest
  auto sources = std::vector<std::filesystem::path>{};
  const auto normalized_output_dir = std::filesystem::absolute(output_dir).lexically_normal();
  for (auto it = std::filesystem::recursive_directory_iterator(assets_dir, error);
       it != std::filesystem::recursive_directory_iterator();
       it.increment(error)) {
    const auto& path = it->path();
    if (it->is_directory()) {
      // this cook's own output, when it lives under the assets
      if (std::filesystem::absolute(path).lexically_normal() == normalized_output_dir) {
        it.disable_recursion_pending();
      }
      continue;
    }

    if (it->is_regular_file()) {
      sources.push_back(path);
    }
  }
  std::ranges::sort(sources);

  // the same mounts the game has, so the manifest's paths are the ones it resolves
  auto vfs = VFS{};
  vfs.mount_dir(VFS::ASSETS_DIR, assets_dir);
  vfs.mount_dir(VFS::COOKED_DIR, output_dir);
  const auto is_shippable = [](const std::filesystem::path& virtual_path) {
    const auto root_dir = *virtual_path.begin();
    return root_dir == VFS::ASSETS_DIR || root_dir == VFS::COOKED_DIR;
  };

  auto manifest = AssetManifest{};
  auto seen = ankerl::unordered_dense::set<UUID>{};
  auto cooked_packs = ankerl::unordered_dense::set<std::string>{};
  auto succeeded = true;
  for (const auto& source : sources) {
    auto result = import_asset(session, output_dir, source);
    // a file the importer doesn't know is not an error, a known one that yields nothing is
    if (!result.uuid && to_asset_type(to_asset_file_type(source)) != AssetType::None) {
      succeeded = false;
    }

    for (auto& asset : result.assets) {
      if (!seen.emplace(asset.uuid).second) {
        continue;
      }

      const auto virtual_path = vfs.to_virtual(asset.path);
      if (!is_shippable(virtual_path)) {
        session.push_error(fmt::format("{} is outside {}, it can't ship.", asset.path, assets_dir));
        succeeded = false;
        continue;
      }

      if (*virtual_path.begin() == VFS::COOKED_DIR) {
        cooked_packs.emplace(virtual_path.lexically_relative(VFS::COOKED_DIR).generic_string());
      }

      manifest.assets.push_back(
        AssetManifest::Entry{
          .uuid = PackedUUID::pack(asset.uuid),
          .type = asset.type,
          .path = virtual_path.generic_string(),
          .source_path = vfs.to_virtual(asset.source_path).generic_string(),
        }
      );

      if (asset.material.has_value()) {
        manifest.materials.push_back(AssetManifest::MaterialEntry::pack(asset.uuid, *asset.material));
      }
    }
  }

  // Only ever `.oxpack`s: the directory is ours, but nothing else in it was put there by a cook. Collected first,
  // removing entries mid-walk is unspecified.
  auto stale_packs = std::vector<std::filesystem::path>{};
  for (const auto& entry : std::filesystem::recursive_directory_iterator(output_dir, error)) {
    const auto& path = entry.path();
    if (
      entry.is_regular_file() && path.extension() == ".oxpack" &&
      !cooked_packs.contains(path.lexically_relative(output_dir).generic_string())
    ) {
      stale_packs.push_back(path);
    }
  }

  for (const auto& path : stale_packs) {
    std::filesystem::remove(path, error);
  }

  if (!manifest.write(output_dir / AssetManifest::FILE_NAME)) {
    session.push_error(fmt::format("Couldn't write the asset manifest into {}.", output_dir));
    return false;
  }

  session.push_message(
    fmt::format("Cooked {} assets ({} packs) into {}.", manifest.assets.size(), cooked_packs.size(), output_dir)
  );

  return succeeded;
}
} // namespace ox::rc
