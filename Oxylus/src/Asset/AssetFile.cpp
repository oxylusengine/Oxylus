#include "Asset/AssetFile.hpp"

#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto AssetType_to_string_view(AssetType type) -> std::string_view {
  ZoneScoped;

  switch (type) {
    case AssetType::None    : return "None";
    case AssetType::Shader  : return "Shader";
    case AssetType::Model   : return "Model";
    case AssetType::Texture : return "Texture";
    case AssetType::Material: return "Material";
    case AssetType::Font    : return "Font";
    case AssetType::Scene   : return "Scene";
    case AssetType::Audio   : return "Audio";
    case AssetType::Script  : return "Script";
    default                 :;
  }

  return "Unknown";
}

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

  entries.resize(header.entry_count);
  if (zpp::bits::failure(deser(zpp::bits::unsized(entries)))) {
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
    .entry_count = static_cast<u32>(self.entries.size()),
  };

  auto [data, ser] = zpp::bits::data_out();
  if (zpp::bits::failure(ser(header, zpp::bits::unsized(self.entries)))) {
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

auto AssetFile::add_entry(this AssetFile& self, ModelData&& entry) -> void {
  self.entries.push_back(AssetFileEntry{.type = AssetType::Model, .data = std::move(entry)});
}

} // namespace ox
