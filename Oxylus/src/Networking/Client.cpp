#include "Networking/Client.hpp"

#include <enet.h>
#include <thread>

namespace ox {

Client::~Client() { auto _ = disconnect(); }

auto Client::set_event_handler(this Client& self, std::shared_ptr<ClientEventHandler> handler) -> Client& {
  ZoneScoped;
  self.event_handler = handler;
  return self;
}

auto Client::set_connect_timeout(this Client& self, u32 timeout_ms) -> Client& {
  ZoneScoped;
  self.connection_timeout = timeout_ms;
  return self;
}

auto Client::set_disconnect_timeout(this Client& self, u32 timeout_ms) -> Client& {
  ZoneScoped;
  self.disconnect_timeout = timeout_ms;
  return self;
}

auto Client::is_connected(this const Client& self) -> bool {
  ZoneScoped;
  return self.state == State::Connected;
}

auto Client::is_connecting(this const Client& self) -> bool {
  ZoneScoped;
  return self.state == State::Connecting;
}

auto Client::get_state(this const Client& self) -> State {
  ZoneScoped;
  return self.state;
}

auto Client::get_enet_server(this const Client& self) -> ENetPeer* {
  ZoneScoped;
  return self.server;
}

auto Client::get_enet_host(this const Client& self) -> ENetHost* {
  ZoneScoped;
  return self.host;
}

auto Client::connect_async(this Client& self, const std::string& host_name, u16 port)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.state != State::Disconnected) {
    return std::unexpected("Client is not in disconnected state");
  }

  self.state = State::Connecting;
  self.connection_start_time = std::chrono::steady_clock::now();

  self.host = enet_host_create(nullptr, 1, 2, 0, 0);
  if (!self.host) {
    self.transition_to_error("Failed to create ENet client host");
    return std::unexpected("Failed to create ENet client host");
  }

  ENetAddress address;
  enet_address_set_host(&address, host_name.c_str());
  address.port = port;

  self.server = enet_host_connect(self.host, &address, 2, 0);
  if (!self.server) {
    enet_host_destroy(self.host);
    self.host = nullptr;
    self.transition_to_error("Failed to initiate connection to server");
    return std::unexpected("Failed to initiate connection to server");
  }

  return {};
}

auto Client::connect(this Client& self, const std::string& host_name, u16 port) -> std::expected<void, std::string> {
  ZoneScoped;

  auto connect_result = self.connect_async(host_name, port);
  if (!connect_result.has_value()) {
    return connect_result;
  }

  // Poll until connected or timeout
  auto start = std::chrono::steady_clock::now();
  while (self.state == State::Connecting) {
    self.update();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                     .count();

    if (elapsed >= self.connection_timeout) {
      enet_peer_reset(self.server);
      enet_host_destroy(self.host);
      self.host = nullptr;
      self.server = nullptr;
      self.transition_to_error("Connection timeout");
      return std::unexpected("Connection timeout");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (self.state == State::Connected) {
    return {};
  }

  return std::unexpected("Connection failed");
}

auto Client::update(this Client& self) -> void {
  ZoneScoped;

  if (!self.host) {
    return;
  }

  using namespace std::chrono;

  // Check for connection timeout in Connecting state
  if (self.state == State::Connecting) {
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - self.connection_start_time).count();

    if (elapsed >= self.connection_timeout) {
      self.transition_to_error("Connection timeout");
      return;
    }
  }

  // Process all pending events (non-blocking)
  ENetEvent event;
  while (enet_host_service(self.host, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        self.handle_connect_event();
        break;
      }

      case ENET_EVENT_TYPE_DISCONNECT: {
        self.handle_disconnect_event();
        break;
      }

      case ENET_EVENT_TYPE_RECEIVE: {
        self.handle_receive_event(event.packet);
        enet_packet_destroy(event.packet);
        break;
      }

      default: break;
    }
  }
}

auto Client::disconnect(this Client& self) -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.state == State::Disconnected) {
    return {};
  }

  if (self.state == State::Error) {
    // Clean up without graceful disconnect
    if (self.host) {
      enet_host_destroy(self.host);
      self.host = nullptr;
    }
    self.server = nullptr;
    self.state = State::Disconnected;
    return {};
  }

  self.state = State::Disconnecting;

  if (self.server) {
    enet_peer_disconnect(self.server, 0);

    // Wait for disconnect acknowledgment
    ENetEvent event;
    auto start = std::chrono::steady_clock::now();

    while (enet_host_service(self.host, &event, 0) > 0 || self.server != nullptr) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                       .count();

      if (elapsed >= self.disconnect_timeout) {
        break; // Timeout - force disconnect
      }

      switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          enet_packet_destroy(event.packet);
          break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
          self.server = nullptr;
          break;
        }
        default: break;
      }
    }

    // Force disconnect if still connected
    if (self.server) {
      enet_peer_reset(self.server);
      self.server = nullptr;
    }
  }

  if (self.host) {
    enet_host_destroy(self.host);
    self.host = nullptr;
  }

  if (self.event_handler) {
    self.event_handler->on_disconnected();
  }

  self.state = State::Disconnected;

  return {};
}

auto Client::send_packet(this Client& self, const Packet& packet) -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.state != State::Connected || !self.server) {
    return std::unexpected("Cannot send packet - not connected");
  }

  std::vector<u8> serialized = packet.serialize();

  ENetPacket* enet_packet = enet_packet_create(serialized.data(), serialized.size(), ENET_PACKET_FLAG_RELIABLE);

  if (enet_peer_send(self.server, 0, enet_packet) < 0) {
    enet_packet_destroy(enet_packet);
    return std::unexpected("Failed to send packet to server");
  }

  return {};
}

auto Client::ping_server(this Client& self) -> void {
  ZoneScoped;

  if (self.server) {
    enet_peer_ping(self.server);
  }
}

auto Client::handle_connect_event() -> void {
  if (state == State::Connecting) {
    state = State::Connected;
    if (event_handler) {
      event_handler->on_connected();
    }
  }
}

auto Client::handle_disconnect_event() -> void {
  bool was_connected = (state == State::Connected);
  state = State::Disconnected;
  server = nullptr;

  if (event_handler) {
    if (was_connected) {
      event_handler->on_disconnected();
    } else {
      event_handler->on_connection_failed("Server refused connection");
    }
  }
}

auto Client::handle_receive_event(ENetPacket* enet_packet) -> void {
  if (state != State::Connected) {
    return; // Ignore packets if not connected
  }

  auto packet = Packet::parse_packet(enet_packet->dataLength, enet_packet->data);
  if (packet.has_value() && event_handler) {
    event_handler->on_packet_received(*packet);
  }
}

auto Client::transition_to_error(const std::string& reason) -> void {
  state = State::Error;
  if (event_handler) {
    event_handler->on_connection_failed(reason);
  }
}

} // namespace ox
