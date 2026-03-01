#include "Networking/NetServer.hpp"

#include <enet.h>

#include "Core/Base.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto NetServer::set_tick_rate(this NetServer& self, f64 tick_rate) -> void {
  ZoneScoped;

  self.tick_interval = 1000.0f / tick_rate;
  self.tick_accum = 0.0f;
}

auto NetServer::tick(this NetServer& self, const Timestep& ts) -> bool {
  ZoneScoped;

  auto event = ENetEvent{};
  while (enet_host_service(self.local_host, &event, 0) > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT: {
        ZoneScopedN("ENET_EVENT_TYPE_CONNECT");
        OX_LOG_INFO("New client(peer: {}) connected.", static_cast<void*>(event.peer));
      } break;
      case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
      case ENET_EVENT_TYPE_DISCONNECT        : {
        ZoneScopedN("ENET_EVENT_TYPE_DISCONNECT");
        auto* remote_peer = event.peer;

        auto client_id = NetClientID::Invalid;
        if (remote_peer->data) {
          client_id = static_cast<NetClientID>(reinterpret_cast<uptr>(remote_peer->data));
        }

        self.on_client_disconnect(client_id);
        self.remote_clients.destroy_slot(client_id);

        event.peer->data = nullptr;
        OX_LOG_INFO("Client(peer: {}) disconnected.", static_cast<void*>(event.peer));
      } break;
      case ENET_EVENT_TYPE_RECEIVE: {
        ZoneScopedN("ENET_EVENT_TYPE_RECEIVE");
        OX_DEFER(&) { enet_packet_destroy(event.packet); };
        auto packet = NetPacket::from_packet(event.packet);
        auto* remote_peer = event.peer;
        if (!packet.has_value()) {
          OX_LOG_ERROR("Received a packet with bad data.");
          break;
        }

        self.handle_packet(remote_peer, packet.value());
      } break;
      case ENET_EVENT_TYPE_NONE: {
      } break;
    }
  }

  self.tick_accum += ts.get_millis();
  if (self.tick_accum >= self.tick_interval) {
    self.tick_accum -= self.tick_interval;
    return true;
  }

  return false;
}

auto NetServer::handle_packet(this NetServer& self, ENetPeer* remote_peer, NetPacket& packet) -> void {
  ZoneScoped;

  auto client_id = NetClientID::Invalid;
  if (remote_peer->data) {
    client_id = static_cast<NetClientID>(reinterpret_cast<uptr>(remote_peer->data));
  }

  switch (packet.type) {
    case NetPacketType::Handshake: {
      auto handshake = packet.get_handshake();
      if (!handshake.has_value()) {
        return;
      }

      // TODO: Actually do some auth checks on client

      // At this point client is accepted
      auto unique_net_id = self.net_id_counter++;
      client_id = self.remote_clients.create_slot(NetClient(remote_peer, unique_net_id));
      remote_peer->data = reinterpret_cast<void*>(static_cast<uptr>(client_id));

      if (auto accept_handshake_packet = NetPacket::handshake({.version = 1, .net_id = unique_net_id})) {
        auto client = self.remote_clients.slot(client_id);
        client->send_reliable(accept_handshake_packet.value());
      }

      self.on_client_connect(client_id);

      OX_LOG_INFO(
        "Accepted new client with NetID = {}, ClientID = (index: {}, version: {})",
        unique_net_id,
        SlotMap_decode_id(client_id).index,
        SlotMap_decode_id(client_id).version
      );
    } break;
    case NetPacketType::SceneSnapshot: {
      // This is not our job
    } break;
    case NetPacketType::ClientAck: {
      auto client_ack = packet.get_client_ack();
      if (!client_ack.has_value()) {
        return;
      }

      self.on_client_ack(client_id, client_ack.value());
    } break;
    case NetPacketType::RPC: {
      auto rpc = packet.get_rpc();
      if (!rpc.has_value()) {
        return;
      }

      auto procs_it = self.rpcs.find(rpc->proc_hash);
      if (procs_it == self.rpcs.end()) {
        OX_LOG_ERROR("Client is trying to call an invalid proc!");
        return;
      }

      procs_it->second(client_id, std::span(rpc->parameters));
    } break;
    case NetPacketType::Unknown: {
      OX_LOG_ERROR("Peer {} sent an unkown packet.");
    } break;
  }
}

auto NetServer::register_proc(this NetServer& self, std::string_view identifier, NetRPCPacket::Callback&& cb) -> void {
  ZoneScoped;

  auto hash = ankerl::unordered_dense::detail::wyhash::hash(identifier.data(), identifier.size());
  self.rpcs.emplace(hash, std::move(cb));
}

} // namespace ox
