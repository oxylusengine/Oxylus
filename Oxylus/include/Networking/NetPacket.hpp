#pragma once

#include <span>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Networking/Fwd.hpp"
#include "Scene/SceneSnapshot.hpp"

namespace ox {
enum class NetPacketType : u8 {
  Unknown = 0,
  Handshake,
  SceneSnapshot,
  ClientAck,
  RPC,
};

// Builtin packets
struct NetHandshakePacket {
  u32 version = 0;
  u64 net_id = ~0_u64;
};

struct NetSceneSnapshotPacket {
  u8 sequence = 0;
  u32 entitiy_count = 0;
  u32 removed_entity_count = 0;
};

struct NetClientAckPacket {
  u8 acked = 0;
};

struct NetRPCPacket {
  struct Parameter {
    u16 data_size = 0;
    void *data = nullptr;
  };

  u64 proc_hash = 0;
  u8 parameter_count = 0;
  u16 total_parameters_size = 0;
  std::span<Parameter> parameters = {};
};

struct NetPacket {
  ENetPacket* inner = nullptr;

  static auto handshake(const NetHandshakePacket& info) -> option<NetPacket>;
  static auto scene_snapshot(SceneState& state, u8 sequence) -> option<NetPacket>;
  static auto client_ack(const NetClientAckPacket& info) -> option<NetPacket>;
  static auto rpc(std::string_view proc, std::span<NetRPCPacket::Parameter> params) -> option<NetPacket>;

  auto destroy(this NetPacket&) -> void;

  auto decr_ref(this NetPacket&) -> usize;
  auto can_destroy(this NetPacket&) -> bool;

  operator ENetPacket*() { return inner; }
};
} // namespace ox
