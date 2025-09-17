#include "Networking/Client.hpp"

namespace ox {
Client::~Client() { auto _ = disconnect(); }

auto Client::set_event_handler(this Client& self, std::shared_ptr<ClientEventHandler> handler) -> Client& {
  self.event_handler_ = handler;
  return self;
}

auto Client::set_connect_timeout(this Client& self, u32 timeout) -> Client& {
  self.connection_timeout_ = timeout;
  return self;
}

auto Client::set_disconnect_timeout(this Client& self, u32 timeout) -> Client& {
  self.disconnect_timeout_ = timeout;
  return self;
}

auto Client::is_connected(this const Client& self) -> bool {
  ZoneScoped;
  return self.state_ == State::Connected;
}

auto Client::get_state(this const Client& self) -> State {
  ZoneScoped;
  return self.state_;
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

  enet_peer_reset(self.server_);
  enet_host_destroy(self.host_);

  self.state_ = State::Error;

  return result;
}

auto Client::request_connection(this Client& self, const std::string& host_name, u16 port)
  -> std::expected<void, std::string> {
  if (self.state_ != State::Disconnected) {
    return std::unexpected("Client is not in disconnected state");
  }

  self.state_ = State::Connecting;

  self.host_ = enet_host_create(nullptr, 1, 2, 0, 0);
  if (!self.host_) {
    return std::unexpected("Failed to create ENet client host");
  }

  ENetAddress address;
  enet_address_set_host(&address, host_name.c_str());
  address.port = port;

  self.server_ = enet_host_connect(self.host_, &address, 2, 0);
  if (!self.server_) {
    enet_host_destroy(self.host_);
    self.host_ = nullptr;
    return std::unexpected("Failed to connect to server");
  }

  return {};
}

auto Client::wait_for_connection(this Client& self) -> std::expected<void, std::string> {
  ENetEvent event;
  if (self.state_ == State::Connected) {
    return {};
  }

  if (enet_host_service(self.host_, &event, self.connection_timeout_) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
    self.state_ = State::Connected;

    if (self.event_handler_) {
      self.event_handler_->on_connected();
    }

    return {};
  }

  return std::unexpected("Server did not respond");
}

auto Client::disconnect(this Client& self) -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.state_ == State::Disconnected || self.state_ == State::Error) {
    return {};
  }

  self.state_ = State::Disconnecting;

  if (self.server_) {
    enet_peer_disconnect(self.server_, 0);

    ENetEvent event;
    while (enet_host_service(self.host_, &event, self.disconnect_timeout_) > 0) {
      switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          enet_packet_destroy(event.packet);
          break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
          self.server_ = nullptr;
          break;
        }
        default: break;
      }
    }

    if (self.server_) {
      enet_peer_reset(self.server_);
    }
  }

  if (self.host_) {
    enet_host_destroy(self.host_);
    self.host_ = nullptr;
  }

  self.remote_peers_.clear();

  if (self.event_handler_) {
    self.event_handler_->on_disconnected();
  }

  self.state_ = State::Disconnected;

  return {};
}

auto Client::update(this Client& self) -> void {
  ZoneScoped;

  if (!self.host_) {
    return;
  }

  ENetEvent event;
  while (enet_host_service(self.host_, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        self.state_ = State::Connected;
        if (self.event_handler_) {
          self.event_handler_->on_connected();
        }
        break;
      }

      case ENET_EVENT_TYPE_DISCONNECT: {
        self.state_ = State::Disconnected;
        if (self.event_handler_) {
          self.event_handler_->on_disconnected();
        }
        break;
      }

      case ENET_EVENT_TYPE_RECEIVE: {
        if (self.state_ == State::Connected) {
          auto packet = Packet::parse_packet(event.packet);
          if (packet.has_value()) {
            if (self.event_handler_) {
              self.event_handler_->on_packet_received(*packet);
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
  if (self.state_ != State::Connected || !self.server_) {
    return std::unexpected("Cannot send packet - not connected");
  }

  std::vector<u8> serialized = packet.serialize();

  // TODO: Configurable packet flag
  ENetPacket* enet_packet = enet_packet_create(serialized.data(), serialized.size(), ENET_PACKET_FLAG_RELIABLE);

  if (enet_peer_send(self.server_, 0, enet_packet) < 0) {
    return std::unexpected("Couldn't send packet to peer");
  }

  return {};
}

auto Client::ping_server(this Client& self) -> void {
  ZoneScoped;

  enet_peer_ping(self.server_);
}

} // namespace ox
