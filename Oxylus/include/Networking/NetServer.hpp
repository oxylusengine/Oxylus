#pragma once

#include "Memory/SlotMap.hpp"
#include "Networking/Fwd.hpp"
#include "Networking/NetClient.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
// `server` is the instance that raised the event. The bus is global, so a handler serving one scene
// has to compare it against its own server before acting.
struct ClientConnectEvent {
  NetServer* server;
  NetClientID client_id;
};
struct ClientDisconnectEvent {
  NetServer* server;
  NetClientID client_id;
};
struct ClientAckEvent {
  NetServer* server;
  NetClientID client_id;
  NetClientAckPacket packet;
};

struct NetServer {
  ENetHost* local_host = nullptr;
  SlotMap<NetClient, NetClientID> remote_clients = {};
  u64 net_id_counter = 0;
  f64 tick_interval = 1000.0f / 20.0f;
  f64 tick_accum = 0.0f;

  ankerl::unordered_dense::map<u64, NetRPCPacket::Callback> rpcs = {};

  NetServer(ENetHost* local_host_) : local_host(local_host_) {};
  virtual ~NetServer() = default;

  auto set_tick_rate(this NetServer&, f64 tick_rate) -> void;
  auto tick(this NetServer&, const Timestep& ts) -> bool;
  auto handle_packet(this NetServer&, ENetPeer* remote_peer, NetPacket& packet) -> void;

  auto register_proc(this NetServer&, std::string_view identifier, NetRPCPacket::Callback&& cb) -> void;

  auto client(this NetServer&, NetClientID client_id) -> NetClient*;
  auto client_ids(this NetServer&) -> std::vector<NetClientID>;

  auto send_to_client(this NetServer&, NetClientID client_id, NetPacket& packet, bool reliable) -> bool;
  auto broadcast(this NetServer&, NetPacket& packet, bool reliable) -> void;

  auto call_client(
    this NetServer&, NetClientID client_id, std::string_view proc, std::span<const RPCParameter> params, bool reliable
  ) -> bool;
  auto broadcast_call(this NetServer&, std::string_view proc, std::span<const RPCParameter> params, bool reliable)
    -> bool;

  virtual auto on_client_connect(NetClientID client_id) -> void {};
  virtual auto on_client_disconnect(NetClientID client_id) -> void {};

  // packets
  virtual auto on_client_ack(NetClientID client_id, NetClientAckPacket& packet) -> void {};
};
} // namespace ox
