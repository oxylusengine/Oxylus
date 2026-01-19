#pragma once

#include <ankerl/svector.h>
#include <span>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Memory/Buffer.hpp"
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

struct RPCParameter {
  enum class Type : u8 {
    None = 0,
    Byte,
    Short,
    Int,
    Int64,
    Float,
    Double,
    String,
    UUID,
    ByteArray,
  };

  Type type = Type::Byte;
  u32 data_size = 0;
  const void* data = nullptr;

  auto as_f32() -> option<const f32>;
  auto as_int64() -> option<const i64>;
  auto as_str() -> std::string_view;
  template <typename T>
  auto as_span() -> std::span<const T> {
    if (type != Type::ByteArray) {
      return {};
    }

    return std::span{static_cast<const T*>(data), data_size / sizeof(T)};
  }
};

struct NetRPCPacket {
  using Callback = std::function<void(NetClientID, std::span<RPCParameter>)>;

  u64 proc_hash = 0;
  ankerl::svector<RPCParameter, 8> parameters = {};
};

struct NetPacket {
  NetPacketType type = NetPacketType::Unknown;
  ENetPacket* inner = nullptr;

  static auto handshake(const NetHandshakePacket& info) -> option<NetPacket>;
  static auto scene_snapshot(SceneState& state, u8 sequence) -> option<NetPacket>;
  static auto client_ack(const NetClientAckPacket& info) -> option<NetPacket>;
  static auto rpc(std::string_view proc, std::span<RPCParameter> params) -> option<NetPacket>;

  static auto from_packet(ENetPacket* packet) -> option<NetPacket>;

  auto destroy(this NetPacket&) -> void;

  auto decr_ref(this NetPacket&) -> usize;
  auto can_destroy(this NetPacket&) -> bool;
  auto reader(this NetPacket&) -> BufferReader;
  auto writer(this NetPacket&) -> BufferWriter;

  auto get_handshake(this NetPacket&) -> option<NetHandshakePacket>;
  auto get_scene_snapshot(this NetPacket&) -> option<std::pair<u8, SceneState>>;
  auto get_client_ack(this NetPacket&) -> option<NetClientAckPacket>;
  auto get_rpc(this NetPacket&) -> option<NetRPCPacket>;

  operator ENetPacket*() { return inner; }
};
} // namespace ox
