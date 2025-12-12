#include "Asset/AssetMetadata.hpp"

#include "OS/File.hpp"

namespace ox {
auto AssetMetadata::from_file(std::filesystem::path& path) -> option<AssetMetadata> {
  auto file = File(path, FileAccess::Read);
  if (!file) {
    return nullopt;
  }

  auto str = std::string{};
  str.resize(file.size);
  file.read(str.data(), file.size);

  return from_string(str);
}

auto AssetMetadata::from_string(std::string_view str) -> option<AssetMetadata> {}
} // namespace ox
