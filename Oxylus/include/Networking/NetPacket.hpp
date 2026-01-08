#pragma once

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Networking/Fwd.hpp"

namespace ox {
enum class NetPacketType : u32 {
  Unknown = 0,
  Handshake,

  // This packet is entirely handled by the "game".
  // There is no additional checks done byNetServer/NetClient.
  Game,
};

enum class NetPacketFlag : u32 {
  None = 0,
  Reliable = 1 << 0,
  Unsequenced = 1 << 1,
  NoAllocate = 1 << 2,
  UnreliableFragment = 1 << 3,
  Unthrottled = 1 << 4,
  Sent = 1 << 8,
};
consteval auto enable_bitmask(NetPacketFlag);

// Builtin packets
struct NetHandshakePacket {
  u32 version = 0;
  u64 net_id = ~0_u64;
};

struct NetPacket {
  ENetPacket* inner = nullptr;

  static auto prepare_raw(NetPacketType type, usize packet_size, const void* initial_data = nullptr)
    -> option<NetPacket>;
  template <typename T>
  static auto prepare(const T& packet, NetPacketType type) -> option<NetPacket> {
    return prepare_raw(type, sizeof(T), &packet);
  }

  auto destroy(this NetPacket&) -> void;
  auto set_flags(this NetPacket&, NetPacketFlag flags) -> NetPacket&;

  auto decr_ref(this NetPacket&) -> usize;
  auto can_destroy(this NetPacket&) -> bool;

  operator ENetPacket*() { return inner; }
};
} // namespace ox
