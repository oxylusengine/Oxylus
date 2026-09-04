#pragma once

#include <span>

#include "ResourceCompiler.hpp"

namespace ox::rc {
enum class TextureSourceKind : u32 {
  Generic = 0, // PNG/JPEG and friends, left for the engine's stb path
  DDS,
  KTX2,
};

auto detect_texture_source_kind(std::span<const u8> bytes) -> TextureSourceKind;

// `bytes` must outlive the call only; the returned mips own their pixels. An unset `srgb` keeps the
// colour space the source declares.
auto compile_texture(Session& session, std::span<const u8> bytes, std::string_view name, option<bool> srgb)
  -> option<TextureData>;
auto compile_texture(Session& session, const TextureCompileRequest& request) -> option<TextureData>;
} // namespace ox::rc
