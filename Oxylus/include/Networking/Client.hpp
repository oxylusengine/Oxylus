#pragma once

#include <chrono>
#include <enet.h>
#include <expected>
#include <memory>
#include <string>

#include "Packet.hpp"

namespace ox {
class ClientEventHandler {
public:
  virtual ~ClientEventHandler() = default;
  virtual auto on_connected() -> void {}
  virtual auto on_disconnected() -> void {}
  virtual auto on_connection_failed(const std::string& reason) -> void {}
  virtual auto on_packet_received(const Packet& packet) -> void {}
};

class Client {
public:
  enum class State { Disconnected, Connecting, Connected, Disconnecting, Error };

  Client() = default;
  ~Client();

  auto set_event_handler(this Client& self, std::shared_ptr<ClientEventHandler> handler) -> Client&;
  auto set_connect_timeout(this Client& self, u32 timeout_ms) -> Client&;
  auto set_disconnect_timeout(this Client& self, u32 timeout_ms) -> Client&;

  // Non-blocking connection - initiates connection, requires update() calls
  auto connect_async(this Client& self, const std::string& host_name, u16 port) -> std::expected<void, std::string>;

  // Blocking connection - calls update() internally
  auto connect(this Client& self, const std::string& host_name, u16 port) -> std::expected<void, std::string>;

  auto update(this Client& self) -> void;

  auto disconnect(this Client& self) -> std::expected<void, std::string>;
  auto send_packet(this Client& self, const Packet& packet) -> std::expected<void, std::string>;
  auto ping_server(this Client& self) -> void;

  [[nodiscard]]
  auto is_connected(this const Client& self) -> bool;
  [[nodiscard]]
  auto is_connecting(this const Client& self) -> bool;
  [[nodiscard]]
  auto get_state(this const Client& self) -> State;
  [[nodiscard]]
  auto get_enet_server(this const Client& self) -> ENetPeer*;
  [[nodiscard]]
  auto get_enet_host(this const Client& self) -> ENetHost*;

private:
  auto handle_connect_event() -> void;
  auto handle_disconnect_event() -> void;
  auto handle_receive_event(ENetPacket* packet) -> void;
  auto transition_to_error(const std::string& reason) -> void;

  ENetHost* host = nullptr;
  ENetPeer* server = nullptr;
  State state = State::Disconnected;

  std::shared_ptr<ClientEventHandler> event_handler = nullptr;

  u32 connection_timeout = 5000;
  u32 disconnect_timeout = 3000;

  // For tracking connection attempt timing
  std::chrono::steady_clock::time_point connection_start_time;
};

} // namespace ox
