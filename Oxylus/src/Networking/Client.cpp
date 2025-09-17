#include "Networking/Client.hpp"

namespace ox {
Client::~Client() { auto _ = disconnect(); }

auto Client::set_event_handler(this Client& self, std::shared_ptr<ClientEventHandler> handler) -> Client& {
  self.event_handler = handler;
  return self;
}

auto Client::set_connect_timeout(this Client& self, u32 timeout) -> Client& {
  self.connection_timeout = timeout;
  return self;
}

auto Client::set_disconnect_timeout(this Client& self, u32 timeout) -> Client& {
  self.disconnect_timeout = timeout;
  return self;
}

auto Client::is_connected(this const Client& self) -> bool {
  ZoneScoped;
  return self.state == State::Connected;
}

auto Client::get_state(this const Client& self) -> State {
  ZoneScoped;
  return self.state;
}

auto Client::connect(this Client& self, const std::string& host_name, u16 port) -> std::expected<void, std::string> {
  ZoneScoped;

  auto request_result = self.request_connection(host_name, port);
  if (!request_result.has_value()) {
    return request_result;
  }

  auto result = self.wait_for_connection();
  if (result.has_value()) {
    return {};
  }

  enet_peer_reset(self.server);
  enet_host_destroy(self.host);

  self.state = State::Error;

  return result;
}

auto Client::request_connection(this Client& self, const std::string& host_name, u16 port)
  -> std::expected<void, std::string> {
  if (self.state != State::Disconnected) {
    return std::unexpected("Client is not in disconnected state");
  }

  self.state = State::Connecting;

  self.host = enet_host_create(nullptr, 1, 2, 0, 0);
  if (!self.host) {
    return std::unexpected("Failed to create ENet client host");
  }

  ENetAddress address;
  enet_address_set_host(&address, host_name.c_str());
  address.port = port;

  self.server = enet_host_connect(self.host, &address, 2, 0);
  if (!self.server) {
    enet_host_destroy(self.host);
    self.host = nullptr;
    return std::unexpected("Failed to connect to server");
  }

  return {};
}

auto Client::wait_for_connection(this Client& self) -> std::expected<void, std::string> {
  ZoneScoped;

  ENetEvent event;
  if (self.state == State::Connected) {
    return {};
  }

  if (enet_host_service(self.host, &event, self.connection_timeout) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
    self.state = State::Connected;

    if (self.event_handler) {
      self.event_handler->on_connected();
    }

    return {};
  }

  return std::unexpected("Server did not respond");
}

auto Client::disconnect(this Client& self) -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.state == State::Disconnected || self.state == State::Error) {
    return {};
  }

  self.state = State::Disconnecting;

  if (self.server) {
    enet_peer_disconnect(self.server, 0);

    ENetEvent event;
    while (enet_host_service(self.host, &event, self.disconnect_timeout) > 0) {
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

    if (self.server) {
      enet_peer_reset(self.server);
    }
  }

  if (self.host) {
    enet_host_destroy(self.host);
    self.host = nullptr;
  }

  self.remote_peers.clear();

  if (self.event_handler) {
    self.event_handler->on_disconnected();
  }

  self.state = State::Disconnected;

  return {};
}

auto Client::update(this Client& self) -> void {
  ZoneScoped;

  if (!self.host) {
    return;
  }

  ENetEvent event;
  while (enet_host_service(self.host, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        self.state = State::Connected;
        if (self.event_handler) {
          self.event_handler->on_connected();
        }
        break;
      }

      case ENET_EVENT_TYPE_DISCONNECT: {
        self.state = State::Disconnected;
        if (self.event_handler) {
          self.event_handler->on_disconnected();
        }
        break;
      }

      case ENET_EVENT_TYPE_RECEIVE: {
        if (self.state == State::Connected) {
          auto packet = Packet::parse_packet(event.packet);
          if (packet.has_value()) {
            if (self.event_handler) {
              self.event_handler->on_packet_received(*packet);
            }
          }
        }
        enet_packet_destroy(event.packet);
        break;
      }

      default: break;
    }
  }
}

auto Client::send_packet(this Client& self, const Packet& packet)
  -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.state != State::Connected || !self.server) {
    return std::unexpected("Cannot send packet - not connected");
  }

  std::vector<u8> serialized = packet.serialize();

  // TODO: Configurable packet flag
  ENetPacket* enet_packet = enet_packet_create(serialized.data(), serialized.size(), ENET_PACKET_FLAG_RELIABLE);

  if (enet_peer_send(self.server, 0, enet_packet) < 0) {
    return std::unexpected("Couldn't send packet to peer");
  }

  return {};
}

auto Client::ping_server(this Client& self) -> void {
  ZoneScoped;

  enet_peer_ping(self.server);
}

} // namespace ox
