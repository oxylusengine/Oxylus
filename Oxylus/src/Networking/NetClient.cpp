#include "Networking/NetClient.hpp"

#include "Core/Base.hpp"
#include "Utils/Log.hpp"

#ifndef ENET_FEATURE_ADDRESS_MAPPING
  #define ENET_FEATURE_ADDRESS_MAPPING
#endif

#include <enet.h>

namespace ox {
auto NetClient::set_tick_rate(this NetClient& self, f64 tick_rate) -> void {
  ZoneScoped;

  self.tick_interval = 1000.0f / tick_rate;
  self.tick_accum = 0.0f;
}

auto NetClient::connect(this NetClient& self, std::string_view host_name, u16 port, f64 timeout) -> bool {
  ZoneScoped;

  auto address = ENetAddress{};
  enet_address_set_host(&address, host_name.data());
  address.port = port;
  self.remote_peer = enet_host_connect(self.local_host, &address, NET_CHANNEL_COUNT, 0);
  if (!self.remote_peer) {
    return false;
  }

  self.status = NetClientStatus::Connecting;
  self.timeout_elapsed = 0.0f;
  self.timeout_max = timeout;

  OX_LOG_INFO("Attempting to connect {}:{}...", host_name, port);

  return true;
}

auto NetClient::disconnect(this NetClient& self, bool immediate, u32 data) -> void {
  ZoneScoped;

  if (!self.remote_peer) {
    return;
  }

  if (immediate) {
    enet_peer_disconnect_now(self.remote_peer, data);
  } else {
    enet_peer_disconnect_later(self.remote_peer, data);
  }

  self.remote_peer = nullptr;
}

auto NetClient::tick(this NetClient& self, const Timestep& ts) -> bool {
  ZoneScoped;

  auto event = ENetEvent{};
  while (enet_host_service(self.local_host, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        ZoneScopedN("ENET_EVENT_TYPE_CONNECT");
        OX_LOG_INFO("NetClient connected.");
        self.status = NetClientStatus::Connected;

        if (auto handshake_packet = NetPacket::handshake({.version = 1})) {
          self.send_reliable(handshake_packet.value());
        }
      } break;
      case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
      case ENET_EVENT_TYPE_DISCONNECT        : {
        ZoneScopedN("ENET_EVENT_TYPE_DISCONNECT");
        OX_LOG_INFO("NetClient disconnected.");

        event.peer->data = nullptr;
      } break;
      case ENET_EVENT_TYPE_RECEIVE: {
        ZoneScopedN("ENET_EVENT_TYPE_RECEIVE");
        OX_DEFER(&) { enet_packet_destroy(event.packet); };

        auto packet = NetPacket::from_packet(event.packet);
        if (!packet.has_value()) {
          OX_LOG_ERROR("Received a packet with bad data.");
          break;
        }

        self.handle_packet(packet.value());
      } break;
      case ENET_EVENT_TYPE_NONE: {
      } break;
    }
  }

  if (self.remote_peer) {
    auto current_sent_bytes = self.remote_peer->totalDataSent;
    auto current_received_bytes = self.remote_peer->totalDataReceived;
    auto current_sent_packets = self.remote_peer->packetsSent;

    self.stats.ping = self.remote_peer->pingInterval;
    self.stats.sent_bytes = current_sent_bytes - self.stats.last_sent_bytes;
    self.stats.received_bytes = current_received_bytes - self.stats.last_received_bytes;
    self.stats.sent_packets = current_sent_packets - self.stats.last_sent_packets;
    self.stats.packets_lost = self.remote_peer->packetsLost;
    self.stats.rtt = self.remote_peer->lastRoundTripTime;
    self.stats.last_sent_bytes = current_sent_bytes;
    self.stats.last_received_bytes = current_received_bytes;
    self.stats.last_sent_packets = current_sent_packets;
  }

  if (self.status == NetClientStatus::Connecting) {
    self.timeout_elapsed += ts.get_millis();

    if (self.timeout_elapsed >= self.timeout_max) {
      OX_LOG_ERROR("Connection attempt timed out after {:.1f}ms", self.timeout_elapsed);

      if (self.remote_peer) {
        enet_peer_reset(self.remote_peer);
        self.remote_peer = nullptr;
      }

      self.status = NetClientStatus::TimedOut;
      self.timeout_elapsed = 0.0;
      return false;
    }
  }

  self.tick_accum += ts.get_millis();
  if (self.tick_accum >= self.tick_interval) {
    self.tick_accum -= self.tick_interval;
    return true;
  }

  return false;
}

auto NetClient::handle_packet(this NetClient& self, NetPacket& packet) -> void {
  ZoneScoped;

  switch (packet.type) {
    case NetPacketType::Handshake: {
      auto handshake = packet.get_handshake();
      if (!handshake.has_value()) {
        return;
      }

      self.net_id = handshake->net_id;
    } break;
    case NetPacketType::SceneSnapshot: {
      auto state = packet.get_scene_snapshot();
      if (!state.has_value()) {
        return;
      }

      self.on_scene_snapshot(state->first, std::move(state->second));
    } break;
    case NetPacketType::ClientAck: {
      // Not our job
    } break;
    case NetPacketType::RPC: {
      auto rpc = packet.get_rpc();
      if (!rpc.has_value()) {
        return;
      }

      auto procs_it = self.rpcs.find(rpc->proc_hash);
      if (procs_it == self.rpcs.end()) {
        OX_LOG_ERROR("Server is trying to call an invalid proc!");
        return;
      }

      procs_it->second(NetClientID::Invalid, std::span(rpc->parameters));
    } break;
    case NetPacketType::Unknown: {
    } break;
  }
}

auto NetClient::add_builtin_procs(this NetClient& self) -> void { ZoneScoped; }

auto NetClient::register_proc(this NetClient& self, std::string_view identifier, NetRPCPacket::Callback&& cb) -> void {
  ZoneScoped;

  auto hash = ankerl::unordered_dense::detail::wyhash::hash(identifier.data(), identifier.size());
  self.rpcs.emplace(hash, std::move(cb));
}

auto NetClient::send_reliable(this NetClient& self, NetPacket& packet) -> void {
  ZoneScoped;

  packet.inner->flags = ENET_PACKET_FLAG_RELIABLE;
  if (enet_peer_send(self.remote_peer, NET_CHANNEL_RELIABLE, packet) < 0) {
    if (packet.can_destroy()) {
      packet.destroy();
    }
  }
}

auto NetClient::send_unreliable(this NetClient& self, NetPacket& packet) -> void {
  ZoneScoped;

  packet.inner->flags = 0;
  if (enet_peer_send(self.remote_peer, NET_CHANNEL_UNRELIABLE, packet) < 0) {
    if (packet.can_destroy()) {
      packet.destroy();
    }
  }
}
} // namespace ox
