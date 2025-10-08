#pragma once

#include <expected>
#include <string>

namespace ox {
class NetworkManager {
public:
  constexpr static auto MODULE_NAME = "NetworkManager";

  auto init() -> std::expected<void, std::string>;
  auto deinit() -> std::expected<void, std::string>;
};
} // namespace ox
