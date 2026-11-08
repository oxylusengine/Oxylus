#pragma once

#include <span>

#include "Core/Types.hpp"

namespace ox {
auto blue_noise_sobol() -> std::span<const u8>;
auto blue_noise_scrambling() -> std::span<const u8>;
} // namespace ox
