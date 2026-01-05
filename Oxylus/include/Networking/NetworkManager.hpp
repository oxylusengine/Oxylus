#pragma once

#include <expected>
#include <string>

#include "Memory/TLSFAllocator.hpp"

namespace ox {
class NetworkManager {
public:
  constexpr static auto MODULE_NAME = "NetworkManager";

  TLSFAllocator allocator = {};

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;
};
} // namespace ox
