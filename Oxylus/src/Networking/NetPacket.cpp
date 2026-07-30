#include "Networking/NetPacket.hpp"

#include <enet.h>

#include "Utils/Log.hpp"

namespace ox {
constexpr auto MAX_PACKET_ALLOC_SIZE = 16_sz * 1024 * 1024;
using SizeOption = zpp::bits::options::size_varint;
using AllocLimitOption = zpp::bits::alloc_limit<MAX_PACKET_ALLOC_SIZE>;

template <typename... T>
auto serialize_packet(NetPacketType type, const T&... payload) -> option<NetPacket> {
  ZoneScoped;

  auto [data, ser] = zpp::bits::data_out(SizeOption{});
  if (zpp::bits::failure(ser(type, payload...))) {
    OX_LOG_ERROR("Failed to serialize packet.");
    return nullopt;
  }

  auto* packet = enet_packet_create(data.data(), data.size(), 0);
  if (!packet) {
    return nullopt;
  }

  return NetPacket{.type = type, .inner = packet};
}

template <typename... T>
auto deserialize_packet(NetPacket& self, NetPacketType type, T&... payload) -> bool {
  ZoneScoped;

  if (self.type != type) {
    return false;
  }

  auto bytes = std::span(self.inner->data, self.inner->dataLength);
  auto deser = zpp::bits::in(bytes, SizeOption{}, AllocLimitOption{});

  // The type is part of the payload, it has already been peeked at by `from_packet`.
  auto packet_type = NetPacketType::Unknown;
  return !zpp::bits::failure(deser(packet_type, payload...));
}

auto RPCParameter::as_f32(this const RPCParameter& self) -> option<const f32> {
  const auto* v = std::get_if<f32>(&self.value);
  if (!v) {
    return nullopt;
  }

  return *v;
}

auto RPCParameter::as_int64(this const RPCParameter& self) -> option<const i64> {
  const auto* v = std::get_if<i64>(&self.value);
  if (!v) {
    return nullopt;
  }

  return *v;
}

auto RPCParameter::as_str(this const RPCParameter& self) -> std::string_view {
  const auto* v = std::get_if<std::string>(&self.value);
  if (!v) {
    return {};
  }

  return *v;
}

auto RPCParameter::as_uuid(this const RPCParameter& self) -> option<UUID> {
  const auto* v = std::get_if<std::array<u8, 16>>(&self.value);
  if (!v) {
    return nullopt;
  }

  auto bytes = *v;
  return UUID::from_bytes(bytes);
}

auto NetPacket::handshake(const NetHandshakePacket& info) -> option<NetPacket> {
  ZoneScoped;

  return serialize_packet(NetPacketType::Handshake, info);
}

auto NetPacket::scene_snapshot(const SceneState& state, u8 sequence) -> option<NetPacket> {
  ZoneScoped;

  return serialize_packet(NetPacketType::SceneSnapshot, sequence, state);
}

auto NetPacket::client_ack(const NetClientAckPacket& info) -> option<NetPacket> {
  ZoneScoped;

  return serialize_packet(NetPacketType::ClientAck, info);
}

auto NetPacket::rpc(std::string_view proc, std::span<const RPCParameter> params) -> option<NetPacket> {
  ZoneScoped;

  const auto proc_hash = ankerl::unordered_dense::detail::wyhash::hash(proc.data(), proc.size());
  return serialize_packet(NetPacketType::RPC, proc_hash, params);
}

auto NetPacket::from_packet(ENetPacket* packet) -> option<NetPacket> {
  ZoneScoped;

  auto bytes = std::span(packet->data, packet->dataLength);
  auto deser = zpp::bits::in(bytes, SizeOption{}, AllocLimitOption{});

  auto packet_type = NetPacketType::Unknown;
  if (zpp::bits::failure(deser(packet_type))) {
    return nullopt;
  }

  return NetPacket{.type = packet_type, .inner = packet};
}

auto NetPacket::destroy(this NetPacket& self) -> void {
  ZoneScoped;

  enet_packet_destroy(self.inner);
}

auto NetPacket::decr_ref(this NetPacket& self) -> usize {
  ZoneScoped;

  if (self.inner->referenceCount == 0) {
    return 0;
  }

  return --self.inner->referenceCount;
}

auto NetPacket::can_destroy(this NetPacket& self) -> bool {
  ZoneScoped;

  return self.inner->referenceCount == 0;
}

auto NetPacket::get_handshake(this NetPacket& self) -> option<NetHandshakePacket> {
  ZoneScoped;

  auto info = NetHandshakePacket{};
  if (!deserialize_packet(self, NetPacketType::Handshake, info)) {
    return nullopt;
  }

  return info;
}

auto NetPacket::get_scene_snapshot(this NetPacket& self) -> option<NetSceneSnapshotPacket> {
  ZoneScoped;

  auto info = NetSceneSnapshotPacket{};
  if (!deserialize_packet(self, NetPacketType::SceneSnapshot, info.sequence, info.state)) {
    return nullopt;
  }

  return info;
}

auto NetPacket::get_client_ack(this NetPacket& self) -> option<NetClientAckPacket> {
  ZoneScoped;

  auto info = NetClientAckPacket{};
  if (!deserialize_packet(self, NetPacketType::ClientAck, info)) {
    return nullopt;
  }

  return info;
}

auto NetPacket::get_rpc(this NetPacket& self) -> option<NetRPCPacket> {
  ZoneScoped;

  auto info = NetRPCPacket{};
  if (!deserialize_packet(self, NetPacketType::RPC, info.proc_hash, info.parameters)) {
    return nullopt;
  }

  return info;
}
} // namespace ox
