#pragma once

#include <string_view>

#include "Core/Types.hpp"
#include "Networking/Fwd.hpp"
#include "Networking/NetPacket.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
enum class NetClientStatus : u32 {
  None = 0,
  Connecting,
  Connected,
  Disconnected,
  TimedOut,
};

struct NetClient {
  NetClientStatus status = NetClientStatus::None;
  NetStats stats = {};
  ENetHost* local_host = nullptr;
  ENetPeer* remote_peer = nullptr;
  u64 net_id = 0;
  f64 timeout_elapsed = 0.0f;
  f64 timeout_max = 0.0f;
  f64 tick_interval = 1000.0f / 20.0f;
  f64 tick_accum = 0.0f;

  ankerl::unordered_dense::map<u64, NetRPCPacket::Callback> rpcs = {};

  NetClient(ENetHost* local_host_) : local_host(local_host_) { add_builtin_procs(); }
  NetClient(ENetPeer* remote_peer_, u64 net_id_) : remote_peer(remote_peer_), net_id(net_id_) { add_builtin_procs(); }
  virtual ~NetClient() = default;

  auto set_tick_rate(this NetClient &, f64 tick_rate) -> void;
  auto connect(this NetClient&, std::string_view host_name, u16 port, f64 timeout) -> bool;
  auto disconnect(this NetClient&, bool immediate, u32 data = 0) -> void;
  auto tick(this NetClient&, const Timestep& ts) -> bool;
  auto handle_packet(this NetClient&, NetPacket& packet) -> void;

  auto add_builtin_procs(this NetClient&) -> void;
  auto register_proc(this NetClient&, std::string_view identifier, NetRPCPacket::Callback&& cb) -> void;

  auto send_reliable(this NetClient&, NetPacket& packet) -> void;
  auto send_unreliable(this NetClient&, NetPacket& packet) -> void;

  virtual auto on_scene_snapshot(u8 sequence, SceneState&& state) -> void {};
};
} // namespace ox
