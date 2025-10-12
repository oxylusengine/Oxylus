#pragma once

#include <ankerl/unordered_dense.h>
#include <expected>
#include <shared_mutex>

#include "Client.hpp"
#include "Networking/Packet.hpp"
#include "Networking/Peer.hpp"

namespace ox {
class ServerEventHandler {
public:
  virtual ~ServerEventHandler() = default;

  virtual auto on_peer_init(Peer& peer) -> void {}
  virtual auto on_peer_connected(const Peer& peer) -> void {}
  virtual auto on_peer_disconnected(const Peer& peer) -> void {}
  virtual auto on_peer_disconnected_timeout(const Peer& peer) -> void {}
  virtual auto on_peer_connect_request(const std::string& playerName, ENetPeer* peer) -> bool { return true; }
  virtual auto on_peer_packet_received(const Peer& peer, const Packet& packet) -> void {}
};

class Server {
public:
  Server() = default;
  ~Server();

  auto set_port(this Server& self, u16 port) -> Server&;
  auto set_max_clients(this Server& self, u32 clients) -> Server&;
  auto set_event_handler(this Server& self, std::shared_ptr<ServerEventHandler> handler) -> Server&;

  auto get_peer(this Server& self, const Client& client) -> const Peer&;
  auto get_peer_count(this Server& self) -> usize;
  auto is_running(this const Server& self) -> bool;

  auto start(this Server& self) -> std::expected<void, std::string>;
  auto stop(this Server& self) -> std::expected<void, std::string>;
  auto update(this Server& self) -> void;
  auto flush(this Server& self) -> void;
  auto send_packet(this Server& self, const Peer& peer, const Packet& packet) -> std::expected<void, std::string>;
  auto send_packet_to_all(this Server& self, const Packet& packet) -> std::expected<void, std::string>;

private:
  static constexpr auto INVALID_PORT = ~0_u16;

  ENetHost* host = nullptr;
  uint16_t port = INVALID_PORT;
  ankerl::unordered_dense::map<usize, Peer> peers = {};
  u32 max_clients = 0;
  bool running = false;
  std::shared_mutex peers_mutex = {};
  std::atomic<usize> next_peer_id = {};
  std::shared_ptr<ServerEventHandler> event_handler = nullptr;

  auto handle_peer_connect(this Server& self, ENetPeer* peer) -> void;
  auto handle_peer_disconnect(this Server& self, ENetPeer* peer) -> void;
  auto handle_peer_disconnect_timeout(this Server& self, ENetPeer* peer) -> void;
  auto handle_peer_packet(this Server& self, ENetPeer* enet_peer, ENetPacket* enet_packet) -> void;
};
} // namespace ox
