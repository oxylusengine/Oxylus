#pragma once

#include <filesystem>
#include <string>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"

namespace ox {
class AssetManager;

namespace rc {
struct Session;
}

// Bumped whenever a compiled payload's meaning changes, so every cache entry in every project goes
// stale at once. `AssetFileHeader::VERSION` covers layout; this covers everything else the compiler
// decides (sRGB choices, LOD thresholds, meshlet limits).
constexpr static auto ASSET_COMPILER_VERSION = 4_u32;

// Alongside the thumbnail cache, and editor-global for the same reason: entries are keyed by UUID,
// so nothing about them is specific to the project that produced them.
auto cache_dir() -> std::filesystem::path;
auto cache_path(const UUID& uuid, std::string_view extension = ".oxpack") -> std::filesystem::path;
auto source_hash(const std::filesystem::path& path) -> u64;

// Whether the engine can read the file as it sits on disk, or the compiler has to cook it first.
auto needs_compiling(const std::filesystem::path& path) -> bool;

// The single funnel every editor import goes through: classifies the file, recompiles it into the
// project cache when the source has moved on, and registers it along with everything under it.
//
// `srgb_directive` is how a model oversees its own resources: a glTF knows a sibling file is a
// normal map, which beats whatever colour space that file labels itself with. A hand-written
// "color_space" in the sidecar still outranks both.
auto import_asset(
  AssetManager& asset_man,
  rc::Session& session,
  const std::filesystem::path& path,
  option<bool> srgb_directive = nullopt
) -> UUID;

// Same, resolving the compiler module itself.
auto import_asset(AssetManager& asset_man, const std::filesystem::path& path, option<bool> srgb_directive = nullopt)
  -> UUID;

// Where a uuid came from on disk. Anything the compiler cooks is registered against the
// `<uuid>.oxpack` in the cache, so `Asset::path` names the pack rather than the file, and the
// sidecar that knows better is keyed by source path -- the import is the only moment both are in
// hand, so it records them here. `name` is what the UI shows: the source file, plus which slot of
// it for the textures and materials a model brings with it. Empty when the uuid was never imported,
// which is every asset whose registry path is already its source.
struct AssetSource {
  std::filesystem::path path = {};
  std::string name = {};
};

auto asset_source(const UUID& uuid) -> AssetSource;
} // namespace ox
