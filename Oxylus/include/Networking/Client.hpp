#pragma once

#include <ankerl/unordered_dense.h>
#include <expected>
#include <shared_mutex>

#include "Networking/Peer.hpp"
#include "Packet.hpp"

namespace ox {
class ClientEventHandler {
public:
  virtual ~ClientEventHandler() = default;

  virtual auto on_connected() -> void {}
  virtual auto on_disconnected() -> void {}
  virtual auto on_packet_received(const Packet& packet) -> void {}
  virtual auto on_peer_connected(const Peer& peer) -> void {}
  virtual auto on_peer_disconnected(const Peer& peer) -> void {}
};

class Client {
public:
  enum class State { Disconnected, Connecting, Connected, Disconnecting, Error };

  Client() = default;
  ~Client();

  auto set_event_handler(this Client& self, std::shared_ptr<ClientEventHandler> handler) -> Client&;
  auto set_connect_timeout(this Client& self, u32 timeout) -> Client&;
  auto set_disconnect_timeout(this Client& self, u32 timeout) -> Client&;

  auto is_connected(this const Client& self) -> bool;
  auto get_state(this const Client& self) -> State;
  auto get_enet_server(this const Client& self) -> ENetPeer*;
  auto get_enet_host(this const Client& self) -> ENetHost*;

  auto connect(this Client& self, const std::string& host_name, u16 port) -> std::expected<void, std::string>;
  auto request_connection(this Client& self, const std::string& host_name, u16 port)
    -> std::expected<void, std::string>;
  auto wait_for_connection(this Client& self) -> std::expected<void, std::string>;
  auto disconnect(this Client& self) -> std::expected<void, std::string>;
  auto update(this Client& self) -> void;
  auto send_packet(this Client& self, const Packet& packet) -> std::expected<void, std::string>;
  auto ping_server(this Client& self) -> void;

private:
  ENetHost* host = nullptr;
  ENetPeer* server = nullptr;
  ankerl::unordered_dense::map<u32, Peer> remote_peers = {};
  std::atomic<State> state = State::Disconnected;
  std::shared_ptr<ClientEventHandler> event_handler = nullptr;
  std::shared_mutex peer_mutex = {};
  u32 connection_timeout = 3000;
  u32 disconnect_timeout = 3000;
};
} // namespace ox
