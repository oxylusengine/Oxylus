#pragma once

#include <filesystem>

#include "Asset/AssetFile.hpp"
#include "Core/Option.hpp"
#include "ResourceCompiler.hpp"

namespace ox::rc {
enum class TextureSourceType { Generic, DDS, KTX };

auto texture_source_type_from_path(const std::filesystem::path& path) -> TextureSourceType;

struct TextureCompiler {
  static auto compile(const TextureCompileRequest& info) -> option<TextureData>;
  static auto compile_from_memory(
    std::span<const u8> bytes,
    const std::string& name,
    bool srgb,
    option<u32> target_width = nullopt,
    option<u32> target_height = nullopt
  ) -> option<TextureData>;
};

} // namespace ox::rc
