#pragma once

#include <filesystem>

#include "Core/Types.hpp"

namespace ox {
auto open_in_external_editor(const std::filesystem::path& file, u32 line = 1, u32 column = 1) -> void;
} // namespace ox
