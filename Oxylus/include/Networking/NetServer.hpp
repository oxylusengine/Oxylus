#pragma once

#include "Core/UUID.hpp"
#include "Memory/SlotMap.hpp"
#include "Networking/Fwd.hpp"
#include "Networking/NetClient.hpp"
#include "Scene/SceneSnapshot.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
struct NetScene {
  UUID scene = {};
  std::vector<NetClientID> clients = {};
};

struct NetServer {
  ENetHost* local_host = nullptr;
  SlotMap<NetClient, NetClientID> remote_clients = {};
  u64 net_id_counter = 0;

  ankerl::unordered_dense::map<NetClientID, SceneSnapshotBuilder> client_snapshots = {};
  ankerl::unordered_dense::map<UUID, NetScene> scenes = {};
  ankerl::unordered_dense::map<NetClientID, usize> client_to_scene = {};

  NetServer(ENetHost* local_host_) : local_host(local_host_) {};
  virtual ~NetServer() = default;

  auto tick(this NetServer&, const Timestep& ts) -> void;
  auto tick_scene(this NetServer&, const Timestep& ts) -> void;
  auto tick_network(this NetServer&, const Timestep& ts) -> void;
  auto handle_net_packet(
    this NetServer&, ENetPeer* remote_peer, NetPacketType type, const void* packet_data, usize packet_size
  ) -> void;

  auto load_scene(this NetServer&, const UUID& uuid) -> bool;

  virtual auto on_client_connect(NetClientID client_id) -> void {};
  virtual auto on_client_disconnect(NetClientID client_id) -> void {};
  virtual auto on_client_rpc(NetClientID client_id, const void* packet_data, usize packet_size) -> void {};
};
} // namespace ox
