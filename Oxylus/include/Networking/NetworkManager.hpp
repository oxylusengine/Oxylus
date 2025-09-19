#pragma once

#include "Core/ESystem.hpp"

namespace ox {
class NetworkManager : public ESystem {
public:
  auto init() -> std::expected<void, std::string> override;
  auto deinit() -> std::expected<void, std::string> override;
};
} // namespace ox
