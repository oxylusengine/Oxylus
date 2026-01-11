#pragma once

namespace ox {
constexpr static auto NET_CHANNEL_COUNT = 2;
constexpr static auto NET_CHANNEL_UNRELIABLE = 0;
constexpr static auto NET_CHANNEL_RELIABLE = 1;
} // namespace ox

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;
typedef struct _ENetEvent ENetEvent;
typedef struct _ENetAddress ENetAddress;
typedef struct _ENetPacket ENetPacket;
