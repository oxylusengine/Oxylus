#pragma once

#include <ankerl/svector.h>
#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <zpp_bits.h>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Core/UUID.hpp"
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
  SceneState state = {};
};

struct NetClientAckPacket {
  u8 acked = 0;
};

struct RPCParameter {
  // The alternative index is what goes over the wire, only ever append to this list.
  using Value = std::variant<
    std::monostate,     // none
    u8,                 // byte
    u16,                // short
    i32,                // int
    i64,                // int64
    f32,                // float
    f64,                // double
    std::string,        // string
    std::array<u8, 16>, // uuid
    std::vector<u8>>;   // byte array

  Value value = {};

  auto as_f32(this const RPCParameter&) -> option<const f32>;
  auto as_int64(this const RPCParameter&) -> option<const i64>;
  auto as_str(this const RPCParameter&) -> std::string_view;
  auto as_uuid(this const RPCParameter&) -> option<UUID>;
  template <typename T>
  auto as_span(this const RPCParameter& self) -> std::span<const T> {
    const auto* bytes = std::get_if<std::vector<u8>>(&self.value);
    if (!bytes) {
      return {};
    }

    return std::span{reinterpret_cast<const T*>(bytes->data()), bytes->size() / sizeof(T)};
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
  static auto scene_snapshot(const SceneState& state, u8 sequence) -> option<NetPacket>;
  static auto client_ack(const NetClientAckPacket& info) -> option<NetPacket>;
  static auto rpc(std::string_view proc, std::span<const RPCParameter> params) -> option<NetPacket>;

  static auto from_packet(ENetPacket* packet) -> option<NetPacket>;

  auto destroy(this NetPacket&) -> void;

  auto decr_ref(this NetPacket&) -> usize;
  auto can_destroy(this NetPacket&) -> bool;

  auto get_handshake(this NetPacket&) -> option<NetHandshakePacket>;
  auto get_scene_snapshot(this NetPacket&) -> option<NetSceneSnapshotPacket>;
  auto get_client_ack(this NetPacket&) -> option<NetClientAckPacket>;
  auto get_rpc(this NetPacket&) -> option<NetRPCPacket>;

  operator ENetPacket*() { return inner; }
};
} // namespace ox
