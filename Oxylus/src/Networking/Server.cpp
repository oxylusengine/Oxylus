#include "Networking/Server.hpp"

#include "Utils/Log.hpp"

namespace ox {
Server::~Server() {
  auto result = stop();
  if (!result) {
    OX_LOG_ERROR("Server stopped with error: {}", result.error());
  }
}

auto Server::set_port(this Server& self, u16 port) -> Server& {
  self.port_ = port;
  return self;
}

auto Server::set_max_clients(this Server& self, u32 clients) -> Server& {
  self.max_clients_ = clients;
  return self;
}

auto Server::set_event_handler(this Server& self, std::shared_ptr<ServerEventHandler> handler) -> Server& {
  self.event_handler_ = handler;
  return self;
}

auto Server::get_peer_count(this Server& self) -> usize {
  ZoneScoped;

  auto read_lock = std::shared_lock(self.peers_mutex);
  return self.peers_.size();
}

auto Server::is_running(this const Server& self) -> bool { return self.running_; }

auto Server::start(this Server& self) -> std::expected<void, std::string> {
  ZoneScoped;

  OX_CHECK_NE(self.max_clients_, 0u, "Max clients can't be 0!");
  OX_CHECK_NE(self.port_, INVALID_PORT, "Port should be set!");

  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = self.port_;

  self.host_ = enet_host_create(&address, self.max_clients_, 2, 0, 0);
  if (!self.host_) {
    return std::unexpected("Failed to create ENet server host!");
  }

  self.running_ = true;
  return {};
}

auto Server::stop(this Server& self) -> std::expected<void, std::string> {
  ZoneScoped;

  self.running_ = false;
  if (self.host_) {
    // disconnect all peers
    for (auto& [id, player] : self.peers_) {
      if (player.peer) {
        enet_peer_disconnect(player.peer, 0);
      }
    }

    // process remaining events
    ENetEvent event;
    while (enet_host_service(self.host_, &event, 100) > 0) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        enet_packet_destroy(event.packet);
      }
    }

    enet_host_destroy(self.host_);
    self.host_ = nullptr;
  }

  self.peers_.clear();

  return {};
}

auto Server::update(this Server& self) -> void {
  ZoneScoped;

  ENetEvent event;
  while (enet_host_service(self.host_, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        self.handle_peer_connect(event.peer);
        break;
      }

      case ENET_EVENT_TYPE_RECEIVE: {
        self.handle_peer_packet(event.peer, event.packet);
        enet_packet_destroy(event.packet);
        break;
      }

      case ENET_EVENT_TYPE_DISCONNECT: {
        self.handle_peer_disconnect(event.peer);
        break;
      }

      case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
        self.handle_peer_disconnect_timeout(event.peer);
        break;
      }

      default: break;
    }
  }
}

auto Server::flush(this Server& self) -> void {
  ZoneScoped;

  enet_host_flush(self.host_);
}

auto Server::send_packet(this Server& self, const Peer& peer, const Packet& packet)
  -> std::expected<void, std::string> {
  ZoneScoped;

  std::vector<u8> serialized = packet.serialize();

  // TODO: Configurable packet flag
  ENetPacket* enet_packet = enet_packet_create(serialized.data(), serialized.size(), ENET_PACKET_FLAG_RELIABLE);

  if (enet_peer_send(peer.peer, 0, enet_packet) < 0) {
    return std::unexpected("Couldn't send packet to peer");
  }

  return {};
}

auto Server::handle_peer_connect(this Server& self, ENetPeer* peer) -> void {
  ZoneScoped;

  u32 peer_id = self.next_peer_id++;
  std::string peer_name = fmt::format("peer_{}", peer_id);

  bool allow_connection = true;
  if (self.event_handler_) {
    allow_connection = self.event_handler_->on_peer_connect_request(peer_name, peer);
  }

  if (!allow_connection) {
    enet_peer_disconnect(peer, 0);
    return;
  }

  auto write_lock = std::unique_lock(self.peers_mutex);
  Peer new_peer(peer_id, peer_name, peer);

  if (self.event_handler_) {
    self.event_handler_->on_peer_init(new_peer);
  }

  new_peer.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(peer_id));

  if (self.event_handler_) {
    self.event_handler_->on_peer_connected(new_peer);
  }

  self.peers_.emplace(peer_id, std::move(new_peer));

  OX_LOG_INFO("Peer connected, {}", peer_id);

  // TODO: broadcast message
}

auto Server::handle_peer_disconnect(this Server& self, ENetPeer* peer) -> void {
  u32 peer_id = static_cast<u32>(reinterpret_cast<uintptr_t>(peer->data));

  auto write_lock = std::unique_lock(self.peers_mutex);
  auto it = self.peers_.find(peer_id);
  if (it != self.peers_.end()) {
    std::string peer_name = it->second.name;
    OX_LOG_INFO("Peer disconnected peer_name:{}", peer_name, peer_id);

    if (self.event_handler_) {
      self.event_handler_->on_peer_disconnected(it->second);
    }

    self.peers_.erase(it);

    // TODO: broadcast message
  }
}

auto Server::handle_peer_disconnect_timeout(this Server& self, ENetPeer* peer) -> void {
  ZoneScoped;

  u32 peer_id = static_cast<u32>(reinterpret_cast<uintptr_t>(peer->data));

  auto write_lock = std::unique_lock(self.peers_mutex);
  auto it = self.peers_.find(peer_id);
  if (it != self.peers_.end()) {
    std::string peer_name = it->second.name;
    OX_LOG_INFO("Peer disconnected because timed out peer_name:{}", peer_name, peer_id);

    if (self.event_handler_) {
      self.event_handler_->on_peer_disconnected_timeout(it->second);
    }

    self.peers_.erase(it);

    // TODO: broadcast message
  }
}

auto Server::handle_peer_packet(this Server& self, ENetPeer* enet_peer, ENetPacket* enet_packet) -> void {
  auto packet = Packet::parse_packet(enet_packet);
  u32 peer_id = static_cast<u32>(reinterpret_cast<uintptr_t>(enet_peer->data));

  // Drop invalid packet
  if (!packet.has_value()) {
    return;
  }

  if (self.event_handler_) {
    auto& peer = self.peers_.at(peer_id);
    self.event_handler_->on_peer_packet_received(peer, *packet);
  }
}
} // namespace ox
