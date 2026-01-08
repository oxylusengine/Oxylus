#pragma once

#include "Memory/SlotMap.hpp"
#include "Networking/Fwd.hpp"
#include "Networking/NetClient.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
struct NetServer {
  ENetHost* local_host = nullptr;
  SlotMap<NetClient, NetClientID> remote_clients = {};
  u64 net_id_counter = 0;

  NetServer(ENetHost* local_host_) : local_host(local_host_) {};
  virtual ~NetServer() = default;

  auto tick(this NetServer&, const Timestep& ts) -> void;
  auto handle_net_packet(
    this NetServer&, ENetPeer* remote_peer, NetPacketType type, const void* packet_data, usize packet_size
  ) -> void;

  virtual auto on_client_connect(NetClientID client_id) -> void {};
  virtual auto on_client_disconnect(NetClientID client_id) -> void {};
  virtual auto on_game_packet(NetClientID client_id, const void* packet_data, usize packet_size) -> void {};
};
} // namespace ox
