#include "Networking/Server.hpp"

#include <enet.h>

#include "Utils/Log.hpp"

namespace ox {
auto NetServer::create(u16 port, u32 max_clients) -> option<NetServer> {
  ZoneScoped;

  auto address = ENetAddress{
    .host = ENET_HOST_ANY,
    .port = port,
    .sin6_scope_id = 0,
  };
  auto* host = enet_host_create(&address, max_clients, 2, 0, 0);
  if (!host) {
    OX_LOG_ERROR("Failed to create new NetServer for port {}!", port);
    return nullopt;
  }

  host->checksum = enet_crc32;

  return NetServer{.host = host};
}

auto NetServer::destroy(this NetServer& self) -> void {
  ZoneScoped;

  self.peer_to_entity.clear();
  self.host = nullptr;

  enet_host_destroy(self.host);
  enet_deinitialize();
}

} // namespace ox
