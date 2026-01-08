#pragma once

#include <string_view>

#include "Core/Types.hpp"
#include "Networking/Fwd.hpp"
#include "Networking/NetPacket.hpp"
#include "Utils/Timestep.hpp"

namespace ox {
enum class NetClientID : u64 { Invalid = ~0_u64 };
enum class NetClientStatus : u32 {
  None = 0,
  Connecting,
  Connected,
  Disconnected,
  TimedOut,
};

struct NetClient {
  NetClientStatus status = NetClientStatus::None;
  ENetHost* local_host = nullptr;
  ENetPeer* remote_peer = nullptr;
  u64 net_id = 0;
  f64 timeout_elapsed = 0.0f;
  f64 timeout_max = 0.0f;

  NetClient(ENetHost* local_host_) : local_host(local_host_) {};
  NetClient(ENetPeer* remote_peer_, u64 net_id_) : remote_peer(remote_peer_), net_id(net_id_) {};
  virtual ~NetClient() = default;

  auto connect(this NetClient&, std::string_view host_name, u16 port, f64 timeout) -> bool;
  auto disconnect(this NetClient&, bool immediate, u32 data = 0) -> void;
  auto tick(this NetClient&, const Timestep& ts) -> void;

  auto send(this NetClient&, NetPacket& packet, NetPacketFlag flags) -> void;
  auto handle_net_packet(this NetClient&, NetPacketType type, const void* packet_data, usize packet_size) -> void;
};
} // namespace ox
