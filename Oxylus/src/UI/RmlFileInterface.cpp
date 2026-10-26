#include "UI/RmlFileInterface.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/Option.hpp"
#include "Core/VFS.hpp"
#include "OS/File.hpp"
#include "Utils/Log.hpp"

namespace ox {
// what a handle points at: the whole file and a cursor into it
struct RmlOpenFile {
  std::string data = {};
  usize position = 0;
};

static auto to_open_file(Rml::FileHandle file) -> RmlOpenFile* { return reinterpret_cast<RmlOpenFile*>(file); }

static auto read_rml_pack(const std::filesystem::path& pack_path) -> option<std::string> {
  auto pack = AssetFile::unpack(pack_path);
  if (!pack) {
    return nullopt;
  }

  for (auto& entry : pack->entries) {
    if (auto* ui_data = std::get_if<UIData>(&entry.data)) {
      return std::move(ui_data->text);
    }

    if (auto* font_data = std::get_if<FontData>(&entry.data)) {
      return std::string(font_data->bytes.begin(), font_data->bytes.end());
    }
  }

  return nullopt;
}

auto resolve_rml_path(const VFS* vfs, std::string_view path) -> std::filesystem::path {
  auto fs_path = std::filesystem::path(path);
  if (!vfs || fs_path.empty() || fs_path.is_absolute() || !vfs->is_mounted_dir(*fs_path.begin())) {
    return fs_path;
  }

  return vfs->to_physical(fs_path);
}

auto find_rml_pack(
  AssetManager& asset_man, const VFS* vfs, std::string_view path, std::initializer_list<AssetType> types
) -> std::filesystem::path {
  ZoneScoped;

  // registered by the virtual path of its source, which is what a virtual document path already is
  const auto uuid = asset_man.find_asset(std::filesystem::path(path));
  if (!uuid) {
    return {};
  }

  auto asset = asset_man.get_asset(uuid);
  if (!asset || std::ranges::find(types, asset->type) == types.end()) {
    return {};
  }

  auto pack_path = vfs ? vfs->to_physical(asset->path) : asset->path;
  return pack_path.extension() == ".oxpack" ? pack_path : std::filesystem::path{};
}

auto read_ui_file(const VFS* vfs, AssetManager* asset_man, std::string_view path) -> option<std::string> {
  ZoneScoped;

  const auto physical_path = resolve_rml_path(vfs, path);
  if (auto contents = File::try_to_string(physical_path); contents.has_value()) {
    return std::move(contents.value());
  }

  if (asset_man) {
    const auto pack_path = find_rml_pack(*asset_man, vfs, path, {AssetType::UI, AssetType::Font});
    if (auto contents = pack_path.empty() ? nullopt : read_rml_pack(pack_path)) {
      return contents;
    }
  }

  // the caller reports the path it asked for, which says nothing about where a virtual one was looked for
  OX_LOG_WARN("Couldn't open '{}' ({}).", path, physical_path);
  return nullopt;
}

auto RmlFileInterface::Open(const Rml::String& path) -> Rml::FileHandle {
  ZoneScoped;

  auto contents = read_ui_file(this->vfs, this->asset_man, path);
  if (!contents.has_value()) {
    return 0;
  }

  auto open_file = std::make_unique<RmlOpenFile>(RmlOpenFile{.data = std::move(contents.value())});
  return reinterpret_cast<Rml::FileHandle>(open_file.release());
}

auto RmlFileInterface::Close(Rml::FileHandle file) -> void {
  ZoneScoped;

  // `Open` released it into the handle
  delete to_open_file(file);
}

auto RmlFileInterface::Read(void* buffer, usize size, Rml::FileHandle file) -> usize {
  ZoneScoped;

  auto* open_file = to_open_file(file);
  const auto count = std::min(size, open_file->data.size() - open_file->position);
  std::memcpy(buffer, open_file->data.data() + open_file->position, count);
  open_file->position += count;

  return count;
}

auto RmlFileInterface::Seek(Rml::FileHandle file, long offset, i32 origin) -> bool {
  ZoneScoped;

  auto* open_file = to_open_file(file);
  auto base = 0_i64;
  switch (origin) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<i64>(open_file->position); break;
    case SEEK_END: base = static_cast<i64>(open_file->data.size()); break;
    default      : return false;
  }

  const auto position = base + static_cast<i64>(offset);
  if (position < 0 || position > static_cast<i64>(open_file->data.size())) {
    return false;
  }

  open_file->position = static_cast<usize>(position);

  return true;
}

auto RmlFileInterface::Tell(Rml::FileHandle file) -> usize { return to_open_file(file)->position; }

auto RmlFileInterface::Length(Rml::FileHandle file) -> usize { return to_open_file(file)->data.size(); }

auto RmlFileInterface::LoadFile(const Rml::String& path, Rml::String& out_data) -> bool {
  ZoneScoped;

  auto contents = read_ui_file(this->vfs, this->asset_man, path);
  if (!contents.has_value()) {
    return false;
  }

  out_data = std::move(contents.value());

  return true;
}
} // namespace ox
