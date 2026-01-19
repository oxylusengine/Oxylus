#pragma once

#include "Core/Types.hpp"

namespace ox {
enum class NetClientID : u64 { Invalid = ~0_u64 };
enum : u32 {
  NET_CHANNEL_UNRELIABLE = 0,
  NET_CHANNEL_RELIABLE,
  NET_CHANNEL_COUNT,
};

struct NetStats {
  u32 ping = 0;
  u32 sent_bytes = 0;
  u32 received_bytes = 0;
  u32 sent_packets = 0;
  u32 packets_lost = 0;
  u32 rtt = 0;

  u32 last_sent_bytes = 0;
  u32 last_received_bytes = 0;
  u32 last_sent_packets = 0;
};
} // namespace ox

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;
typedef struct _ENetEvent ENetEvent;
typedef struct _ENetAddress ENetAddress;
typedef struct _ENetPacket ENetPacket;
