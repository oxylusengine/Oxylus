#include "Networking/NetServer.hpp"

#include <enet.h>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Base.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto NetServer::tick(this NetServer& self, const Timestep& ts) -> void {
  ZoneScoped;

  if (App::has_mod<AssetManager>()) {
    self.tick_scene(ts);
  }

  self.tick_network(ts);
}

auto NetServer::tick_scene(this NetServer& self, const Timestep& ts) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();

  // I think it's better to first update all scenes
  for (const auto& [scene_uuid, _] : self.scenes) {
    auto* scene = asset_man.get_scene(scene_uuid);
    scene->runtime_update(ts);
  }

  for (const auto& [scene_uuid, net_scene] : self.scenes) {
    auto* scene = asset_man.get_scene(scene_uuid);
    auto tick_state = SceneState{};
    SceneSnapshotBuilder::take_snapshot(scene->world, tick_state);
    for (const auto& client_id : net_scene.clients) {
      auto& [_, client_snapshot] = *self.client_snapshots.find(client_id);
      auto* client = self.remote_clients.slot(client_id);

      client_snapshot.set_current(tick_state);
      auto delta_state = client_snapshot.delta();
      if (auto delta_state_packet = NetPacket::scene_snapshot(delta_state, client_snapshot.current_sequence)) {
        client->send_unreliable(*delta_state_packet);
      }

      client_snapshot.advance();
    }
  }
}

auto NetServer::tick_network(this NetServer& self, const Timestep& ts) -> void {
  ZoneScoped;

  auto event = ENetEvent{};
  if (enet_host_service(self.local_host, &event, 0) > 0) {
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
        auto* packet = event.packet;
        auto* remote_peer = event.peer;

        if (packet->dataLength < sizeof(NetPacketType)) {
          OX_LOG_ERROR("Received a packet with bad data.");
          return;
        }

        auto packet_type = *reinterpret_cast<NetPacketType*>(packet->data);
        self.handle_net_packet(
          remote_peer,
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
}

auto NetServer::handle_net_packet(
  this NetServer& self, ENetPeer* remote_peer, NetPacketType type, const void* packet_data, usize packet_size
) -> void {
  ZoneScoped;

  auto client_id = NetClientID::Invalid;
  if (remote_peer->data) {
    client_id = static_cast<NetClientID>(reinterpret_cast<uptr>(remote_peer->data));
  }

  switch (type) {
    case NetPacketType::Handshake: {
      if (packet_size < sizeof(NetHandshakePacket)) {
        break;
      }
      auto& handshake_packet = *static_cast<const NetHandshakePacket*>(packet_data);
      // TODO: Actually do some auth checks on client

      // At this point client is accepted
      auto unique_net_id = self.net_id_counter++;
      auto new_client_id = self.remote_clients.create_slot(NetClient(remote_peer, unique_net_id));
      remote_peer->data = reinterpret_cast<void*>(static_cast<uptr>(new_client_id));

      if (auto accept_handshake_packet = NetPacket::handshake({.version = 1, .net_id = unique_net_id})) {
        auto client = self.remote_clients.slot(new_client_id);
        client->send_reliable(accept_handshake_packet.value());
      }

      self.on_client_connect(client_id);

      OX_LOG_INFO(
        "Accepted new client with NetID = {}, ClientID = (index: {}, version: {})",
        unique_net_id,
        SlotMap_decode_id(new_client_id).index,
        SlotMap_decode_id(new_client_id).version
      );
    } break;
    case NetPacketType::SceneSnapshot: {
      // This is not our job
    } break;
    case NetPacketType::ClientAck: {
      if (packet_size < sizeof(NetClientAckPacket)) {
        break;
      }

      const auto* packet = static_cast<const NetClientAckPacket*>(packet_data);
      auto client_snapshot_it = self.client_snapshots.find(client_id);
      if (client_snapshot_it == self.client_snapshots.end()) {
        // TODO, BUG: This is very illegal, should probably report this?
        break;
      }

      auto& [_, state] = *client_snapshot_it;
      state.ack(packet->acked);
    } break;
    case NetPacketType::RPC: {
      self.on_client_rpc(client_id, packet_data, packet_size);
    } break;
    case NetPacketType::Unknown: {
      OX_LOG_ERROR("Peer {} sent an unkown packet.");
    } break;
  }
}

auto NetServer::load_scene(this NetServer& self, const UUID& uuid) -> bool {
  ZoneScoped;

  if (self.scenes.contains(uuid)) {
    // We already have refcounting inside assetman but...
    return true;
  }

  auto& asset_man = App::mod<AssetManager>();
  if (!asset_man.load_asset(uuid)) {
    return false;
  }

  self.scenes.emplace(uuid, NetScene{.scene = uuid});

  return true;
}

} // namespace ox
