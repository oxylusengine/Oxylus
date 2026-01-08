#pragma once

#include <ankerl/svector.h>
#include <expected>
#include <memory>
#include <string>

#include "Core/Types.hpp"
#include "Memory/TLSFAllocator.hpp"
#include "Networking/NetClient.hpp"
#include "Networking/NetServer.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
struct NetServer;
struct NetClient;

enum class NetEventKind : u32 {
  Connect = 0,
  Receive,
  Disconnect,
};

struct NetworkManager {
  constexpr static auto MODULE_NAME = "NetworkManager";

  TLSFAllocator allocator = {};
  ankerl::svector<std::unique_ptr<NetServer>, 1> servers = {};
  ankerl::svector<std::unique_ptr<NetClient>, 1> clients = {};

  auto init(this NetworkManager&) -> std::expected<void, std::string>;
  auto deinit(this NetworkManager&) -> std::expected<void, std::string>;
  auto update(this NetworkManager&, const Timestep& timestep) -> void;

private:
  auto create_server_handle(this NetworkManager&, u16 port, u32 max_clients) -> ENetHost*;
  auto create_client_handle(this NetworkManager&) -> ENetHost*;

public:
  template <typename T = NetServer>
  auto create_server(this NetworkManager& self, u16 port, u32 max_clients) -> T* {
    auto* host = self.create_server_handle(port, max_clients);
    if (!host) {
      return nullptr;
    }

    auto server = std::make_unique<T>(host);
    auto server_ptr = server.get();
    self.servers.emplace_back(std::move(server));

    return server_ptr;
  }

  template <typename T = NetClient>
  auto create_client(this NetworkManager& self) -> T* {
    auto* host = self.create_client_handle();
    if (!host) {
      return nullptr;
    }

    auto client = std::make_unique<T>(host);
    auto client_ptr = client.get();
    self.clients.emplace_back(std::move(client));

    return client_ptr;
  }

  auto destroy_server(this NetworkManager&, NetServer*& server) -> void;
  auto destroy_client(this NetworkManager&, NetClient*& client) -> void;
};
} // namespace ox
