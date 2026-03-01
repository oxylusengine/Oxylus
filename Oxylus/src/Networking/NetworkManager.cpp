#include "Networking/NetworkManager.hpp"

#include <enet.h>

#include "Utils/Log.hpp"

namespace ox {
auto NetworkManager::init(this NetworkManager&) -> std::expected<void, std::string> {
  ZoneScoped;

  // TODO: There is also `_with_callbacks` version to controll allocations ourselves
  if (enet_initialize() != 0) {
    return std::unexpected("An error occurred while initializing ENet");
  }

  return {};
}

auto NetworkManager::deinit(this NetworkManager& self) -> std::expected<void, std::string> {
  ZoneScoped;

  OX_ASSERT(self.servers.empty());
  OX_ASSERT(self.clients.empty());

  enet_deinitialize();

  return {};
}

auto NetworkManager::update(this NetworkManager&, const Timestep&) -> void { ZoneScoped; }

auto NetworkManager::create_server_handle(this NetworkManager& self, u16 port, u32 max_clients) -> ENetHost* {
  ZoneScoped;

  auto address = ENetAddress{
    .host = ENET_HOST_ANY,
    .port = port,
    .sin6_scope_id = 0,
  };
  auto* local_host = enet_host_create(&address, max_clients, NET_CHANNEL_COUNT, 0, 0);
  if (!local_host) {
    OX_LOG_ERROR("Failed to create new NetServer for port {}!", port);
    return nullptr;
  }

  // TODO: Compression
  local_host->checksum = enet_crc32;

  OX_LOG_INFO("NetServer listening for port {}.", port);

  return local_host;
}

auto NetworkManager::create_client_handle(this NetworkManager& self) -> ENetHost* {
  ZoneScoped;

  auto* local_host = enet_host_create(nullptr, 1, NET_CHANNEL_COUNT, 0, 0);
  if (!local_host) {
    OX_LOG_ERROR("Failed to create new NetClient!");
    return nullptr;
  }

  // TODO: Compression
  local_host->checksum = enet_crc32;

  return local_host;
}

auto NetworkManager::destroy_server(this NetworkManager& self, NetServer* server) -> void {
  ZoneScoped;

  enet_host_destroy(server->local_host);
  std::erase_if(self.servers, [&](std::unique_ptr<NetServer>& v) { return v.get() == server; });

  server = nullptr;
}

auto NetworkManager::destroy_client(this NetworkManager& self, NetClient* client) -> void {
  ZoneScoped;

  client->disconnect(true);
  enet_host_destroy(client->local_host);
  std::erase_if(self.clients, [&](std::unique_ptr<NetClient>& v) { return v.get() == client; });

  client = nullptr;
}

} // namespace ox
