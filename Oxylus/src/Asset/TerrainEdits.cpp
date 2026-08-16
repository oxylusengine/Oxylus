#include "Asset/TerrainEdits.hpp"

#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
constexpr u32 TERRAIN_EDITS_MAGIC = 0x5254584f; // "OXTR"

// The maps are a f32 height delta and an RGBA8 splat override, so both are four bytes per texel.
constexpr u64 TERRAIN_EDITS_TEXEL_SIZE = 4;

struct TerrainEditsHeader {
  u32 magic = TERRAIN_EDITS_MAGIC;
  u32 width = 0;
  u32 height = 0;
  u32 padding = 0;
};

auto TerrainEdits::read(const std::filesystem::path& path) -> option<TerrainEdits> {
  ZoneScoped;

  if (!std::filesystem::exists(path)) {
    return nullopt;
  }

  auto file = File(path, FileAccess::Read);
  if (!file) {
    return nullopt;
  }

  auto header = TerrainEditsHeader{};
  if (file.read(&header, sizeof(header)) != sizeof(header) || header.magic != TERRAIN_EDITS_MAGIC) {
    OX_LOG_ERROR("'{}' is not a terrain edits file.", path);
    return nullopt;
  }

  const auto map_bytes = static_cast<u64>(header.width) * header.height * TERRAIN_EDITS_TEXEL_SIZE;
  if (map_bytes == 0 || file.size != sizeof(header) + 2 * map_bytes) {
    OX_LOG_ERROR("Terrain edits file '{}' does not hold a {}x{} pair of maps.", path, header.width, header.height);
    return nullopt;
  }

  auto edits = TerrainEdits{
    .resolution = {header.width, header.height},
    .height = std::vector<u8>(map_bytes),
    .splat = std::vector<u8>(map_bytes),
  };

  if (file.read(edits.height.data(), map_bytes) != map_bytes || file.read(edits.splat.data(), map_bytes) != map_bytes) {
    OX_LOG_ERROR("Terrain edits file '{}' is truncated.", path);
    return nullopt;
  }

  return edits;
}

auto TerrainEdits::write(this const TerrainEdits& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  const auto header = TerrainEditsHeader{.width = self.resolution.x, .height = self.resolution.y};

  auto file = File(path, FileAccess::Write);
  if (!file) {
    return false;
  }

  file.write_data(&header, sizeof(header));
  file.write(self.height);
  file.write(self.splat);

  return true;
}
} // namespace ox
