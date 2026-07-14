#pragma once

#include <filesystem>

#include "Asset/AssetFile.hpp"
#include "Core/Option.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
enum class TextureSourceType { Generic, DDS, KTX };

auto texture_source_type_from_path(const std::filesystem::path& path) -> TextureSourceType;

struct TextureCompiler {
  static auto compile(const TextureCompileInfo& info) -> option<TextureData>;
};

} // namespace ox::rc
