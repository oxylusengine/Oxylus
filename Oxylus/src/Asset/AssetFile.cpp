#include "Asset/AssetFile.hpp"

#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto AssetFile::unpack(const std::filesystem::path& path) -> option<AssetFile> {
  ZoneScoped;

  auto file = File(path, FileAccess::Read);
  auto* mapped_data = file.map();
  auto bytes = std::span(static_cast<u8*>(mapped_data), file.size);
  auto deser = zpp::bits::in(bytes);

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

  if (zpp::bits::failure(deser(entries))) {
    OX_LOG_ERROR("Failed to deserialize Asset entries.");
    return nullopt;
  }

  return AssetFile{
    .flags = header.flags,
    .entries = entries,
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

auto AssetFile::add_entry(this AssetFile& self, ShaderPipelineData&& entry) -> void {
  ZoneScoped;

  self.entries.push_back(
    AssetFileEntry{
      .type = AssetType::Shader,
      .data = std::move(entry),
    }
  );
}

auto AssetFile::add_entry(this AssetFile& self, TextureData&& entry) -> void {
  self.entries.push_back(AssetFileEntry{.type = AssetType::Texture, .data = std::move(entry)});
}

} // namespace ox
