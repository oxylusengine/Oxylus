#pragma once

#include <span>

#include "ResourceCompiler.hpp"

namespace ox::rc {
// `bytes` must outlive the call only; the returned mips own their pixels. An unset `usage` is inferred from the
// source, see `TextureCompileRequest::usage`.
auto compile_texture(Session& session, std::span<const u8> bytes, std::string_view name, option<TextureUsage> usage)
  -> option<TextureData>;
auto compile_texture(Session& session, const TextureCompileRequest& request) -> option<TextureData>;
} // namespace ox::rc
