#include "Asset/AssetFile.hpp"

#include <algorithm>

#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
// caps any single length-prefixed container in a pack; the largest thing we store is a mesh blob
constexpr static auto MAX_ENTRY_ELEMENTS = 1_u64 << 31;

auto PackedUUID::pack(const UUID& uuid) -> PackedUUID {
  auto self = PackedUUID{};
  std::ranges::copy(uuid.bytes(), self.bytes.begin());
  return self;
}

auto PackedUUID::unpack(this const PackedUUID& self) -> UUID {
  auto bytes = self.bytes;
  return UUID::from_bytes(bytes).value_or(UUID(nullptr));
}

auto AssetFile::unpack(const std::filesystem::path& path) -> option<AssetFile> {
  ZoneScoped;

  auto file = File(path, FileAccess::Read);
  auto* mapped_data = file.map();
  auto bytes = std::span(static_cast<u8*>(mapped_data), file.size);
  // a pack is untrusted input: a corrupt length prefix must not turn into a huge allocation
  auto deser = zpp::bits::in(bytes, zpp::bits::alloc_limit<MAX_ENTRY_ELEMENTS>{});

  auto header = AssetFileHeader{};
  auto entries = std::vector<AssetFileEntry>();

  if (zpp::bits::failure(deser(header))) {
    OX_LOG_ERROR("Failed to deserialize Asset Header.");
    return nullopt;
  }

  if (header.magic != AssetFileHeader::SIGNATURE) {
    OX_LOG_ERROR("Failed to deserialize Asset Header. Signatures don't match.");
    return nullopt;
  }

  if (header.version != AssetFileHeader::VERSION) {
    OX_LOG_ERROR(
      "Asset file '{}' was built with version {}, expected {}. Recompile it.",
      path,
      header.version,
      AssetFileHeader::VERSION
    );
    return nullopt;
  }

  if (zpp::bits::failure(deser(entries))) {
    OX_LOG_ERROR("Failed to deserialize Asset entries.");
    return nullopt;
  }

  return AssetFile{
    .flags = header.flags,
    .entries = std::move(entries),
  };
}

auto AssetFile::pack(this AssetFile& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  auto header = AssetFileHeader{
    .flags = self.flags,
  };

  auto [data, ser] = zpp::bits::data_out();
  if (zpp::bits::failure(ser(header, self.entries))) {
    OX_LOG_ERROR("Failed to serialize asset file.");
    return false;
  }

  auto file = File(path, FileAccess::Write);
  if (!file) {
    OX_LOG_ERROR("Failed to open '{}' for writing.", path);
    return false;
  }

  file.write(data);

  return true;
}

auto AssetFile::add_entry(this AssetFile& self, ShaderPipelineData&& entry, const PackedUUID& uuid) -> void {
  ZoneScoped;

  self.entries.push_back(
    AssetFileEntry{
      .uuid = uuid,
      .type = AssetType::Shader,
      .data = std::move(entry),
    }
  );
}

auto AssetFile::add_entry(this AssetFile& self, TextureData&& entry, const PackedUUID& uuid) -> void {
  ZoneScoped;

  self.entries.push_back(
    AssetFileEntry{
      .uuid = uuid,
      .type = AssetType::Texture,
      .data = std::move(entry),
    }
  );
}

auto AssetFile::add_entry(this AssetFile& self, ModelData&& entry, const PackedUUID& uuid) -> void {
  ZoneScoped;

  self.entries.push_back(
    AssetFileEntry{
      .uuid = uuid,
      .type = AssetType::Model,
      .data = std::move(entry),
    }
  );
}

} // namespace ox
