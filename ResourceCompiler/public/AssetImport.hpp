#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Asset/Material.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"
#include "ResourceCompiler.hpp"

namespace ox {
struct JsonWriter;
}

namespace ox::rc {
// Bumped whenever a compiled payload's meaning changes, so every cooked pack everywhere goes stale at once.
// `AssetFileHeader::VERSION` covers layout; this covers everything else the compiler decides (sRGB choices, LOD
// thresholds, meshlet limits).
constexpr static auto ASSET_COMPILER_VERSION = 4_u32;

// Project file formats the importer knows how to turn into assets.
enum class AssetFileType : u32 {
  None = 0,
  Binary,
  Meta,
  GLB,
  GLTF,
  PNG,
  JPEG,
  DDS,
  JSON,
  KTX2,
  LUA,
  OXTERRAIN,
  OXPARTICLE,
  WAV,
  MP3,
  FLAC,
  OGG,
};

OXRC_API auto to_asset_file_type(const std::filesystem::path& path) -> AssetFileType;
OXRC_API auto to_asset_type(AssetFileType file_type) -> AssetType;
// whether the engine can read the file as it sits on disk, or it has to be cooked into an `.oxpack` first
OXRC_API auto needs_compiling(AssetFileType file_type) -> bool;
OXRC_API auto needs_compiling(const std::filesystem::path& path) -> bool;
OXRC_API auto meta_file_path(const std::filesystem::path& path) -> std::filesystem::path;
OXRC_API auto owns_meta_file(const std::filesystem::path& path) -> bool;
// of the contents, so it is the same on every machine and a fresh checkout doesn't look stale
OXRC_API auto source_hash(const std::filesystem::path& path) -> u64;

// Sidecar primitives, public so the editor's save path writes exactly the format the importer reads.
// `begin_asset_meta` writes `uuid` then `type`, and sidecars are read back single pass, so anything appended must be
// read in the order it is written. `end_asset_meta` leaves an identical sidecar untouched.
OXRC_API auto begin_asset_meta(JsonWriter& writer, const UUID& uuid, AssetType type) -> void;
OXRC_API auto end_asset_meta(JsonWriter& writer, const std::filesystem::path& path) -> bool;
OXRC_API auto write_material_asset_meta(JsonWriter& writer, const UUID& uuid, const Material& material) -> bool;

// One asset an import produced, for the caller to register: the editor into its `AssetManager`, `cook_assets` into
// an `AssetManifest`. Paths are physical.
struct ImportedAsset {
  UUID uuid = UUID(nullptr);
  AssetType type = AssetType::None;
  // where the payload loads from, a pack in the cooked dir for anything compiled
  std::filesystem::path path = {};
  // the file this asset was imported from, empty for the materials and embedded textures a model brings along
  std::filesystem::path source_path = {};
  // the file it came out of even when that isn't its own source, and a name for it, for the editor's UI
  std::filesystem::path origin = {};
  std::string name = {};
  // a material's definition, which lives in a sidecar rather than a payload file
  option<Material> material = nullopt;
};

struct ImportResult {
  // the asset for the imported file itself, null when it isn't an asset or the import failed
  UUID uuid = UUID(nullptr);
  // everything registered along the way, including a model's sub-assets and the sibling textures it pulled in
  std::vector<ImportedAsset> assets = {};
};

// Classifies `path`, cooks it into `cooked_dir` when its source moved on since the last cook there, and writes or
// updates its sidecar, creating one (and so a UUID) for a file that has none. Safe to call from several threads, a
// file reached twice at once is cooked once. Failures are pushed to the session's diagnostics.
//
// `srgb_directive` is how a model oversees its own resources: a glTF knows a sibling file is a normal map, which
// beats whatever colour space that file labels itself with. A hand-written "color_space" in the sidecar still
// outranks both.
OXRC_API auto import_asset(
  Session& session,
  const std::filesystem::path& cooked_dir,
  const std::filesystem::path& path,
  option<bool> srgb_directive = nullopt
) -> ImportResult;

// What a game build runs instead of the editor: imports everything under `assets_dir`, cooking into `output_dir`,
// removes packs there that no asset uses anymore, and writes the `AssetManifest` the game registers its assets from.
// `output_dir` is mounted as `VFS::COOKED_DIR` at runtime, so it must be dedicated to this.
OXRC_API auto cook_assets(
  Session& session, const std::filesystem::path& assets_dir, const std::filesystem::path& output_dir
) -> bool;
} // namespace ox::rc
