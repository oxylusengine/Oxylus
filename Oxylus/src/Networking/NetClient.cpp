#include "Networking/NetClient.hpp"

#include "Core/Base.hpp"
#include "Utils/Log.hpp"

#ifndef ENET_FEATURE_ADDRESS_MAPPING
  #define ENET_FEATURE_ADDRESS_MAPPING
#endif

#include <enet.h>

namespace ox {
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

  if (immediate) {
    enet_peer_disconnect_now(self.remote_peer, data);
  } else {
    enet_peer_disconnect_later(self.remote_peer, data);
  }

  self.remote_peer = nullptr;
}

auto NetClient::tick(this NetClient& self, const Timestep& ts) -> void {
  ZoneScoped;

  auto event = ENetEvent{};
  if (enet_host_service(self.local_host, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        ZoneScopedN("ENET_EVENT_TYPE_CONNECT");
        OX_LOG_INFO("NetClient connected.");
        self.status = NetClientStatus::Connected;

        if (auto handshake_packet = NetPacket::prepare<NetHandshakePacket>({.version = 1}, NetPacketType::Handshake)) {
          self.send(handshake_packet.value(), NetPacketFlag::Reliable);
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

        auto* packet = event.packet;
        if (packet->dataLength < sizeof(NetPacketType)) {
          OX_LOG_ERROR("Received a packet with bad data.");
          return;
        }

        auto packet_type = *reinterpret_cast<NetPacketType*>(packet->data);
        self.handle_net_packet(
          packet_type,
          packet->data + sizeof(NetPacketType),
          packet->dataLength - sizeof(NetPacketType)
        );

        return;
      }
      case ENET_EVENT_TYPE_NONE: {
      } break;
    }
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
      return;
    }
  }
}

auto NetClient::send(this NetClient& self, NetPacket& packet, NetPacketFlag flags) -> void {
  ZoneScoped;

  packet.set_flags(flags);

  if (enet_peer_send(self.remote_peer, 0, packet) < 0) {
    if (packet.can_destroy()) {
      packet.destroy();
    }
  }
}

auto NetClient::handle_net_packet(this NetClient& self, NetPacketType type, const void* packet_data, usize packet_size)
  -> void {
  ZoneScoped;

  switch (type) {
    case NetPacketType::Handshake: {
      if (packet_size < sizeof(NetHandshakePacket)) {
        return;
      }
      auto& handshake_packet = *static_cast<const NetHandshakePacket*>(packet_data);
      self.net_id = handshake_packet.net_id;
      OX_LOG_INFO("{}", self.net_id);
    } break;
    case NetPacketType::Unknown: {
    } break;
  }
}
} // namespace ox
