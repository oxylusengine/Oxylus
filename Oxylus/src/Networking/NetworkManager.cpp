#include "Networking/NetworkManager.hpp"

#include <enet.h>

namespace ox {
auto NetworkManager::init() -> std::expected<void, std::string> {
  ZoneScoped;

  if (enet_initialize() != 0) {
    return std::unexpected("An error occurred while initializing ENet");
  }

  return {};
}

auto NetworkManager::deinit() -> std::expected<void, std::string> {
  ZoneScoped;
  enet_deinitialize();

  return {};
}
} // namespace ox
