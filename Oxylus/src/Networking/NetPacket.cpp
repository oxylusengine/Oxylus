#include "Networking/NetPacket.hpp"

#include <enet.h>
#include <utility>

#include "Memory/Buffer.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto new_packet(usize packet_size, u32 flags) -> ENetPacket* {
  ZoneScoped;

  return enet_packet_create(nullptr, packet_size + sizeof(NetPacketType), flags);
}

auto NetPacket::handshake(const NetHandshakePacket& info) -> option<NetPacket> {
  ZoneScoped;

  auto packet_size = 0;
  packet_size += sizeof(NetHandshakePacket::version);
  packet_size += sizeof(NetHandshakePacket::net_id);
  auto* packet = new_packet(packet_size, ENET_PACKET_FLAG_RELIABLE);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  buf.write(NetPacketType::Handshake);
  buf.write(info.version);
  buf.write(info.net_id);

  OX_ASSERT(buf.remaining() == 0);

  return NetPacket{.inner = packet};
}

auto NetPacket::scene_snapshot(SceneState& state, u8 sequence) -> option<NetPacket> {
  ZoneScoped;

  auto packet_size = 0;
  packet_size += sizeof(NetSceneSnapshotPacket::sequence);
  packet_size += sizeof(NetSceneSnapshotPacket::entitiy_count);
  packet_size += sizeof(NetSceneSnapshotPacket::removed_entity_count);
  packet_size += state.removed_entities.size() * sizeof(flecs::entity_t);

  // `entities`
  for (const auto& [_, entity_state] : state.entities) {
    packet_size += sizeof(flecs::entity_t); // `entity_id`
    packet_size += sizeof(u16);             // `components` count
    packet_size += sizeof(u16);             // `removed_components` count
    packet_size += entity_state.components.size() * sizeof(flecs::id_t);

    // `components`
    for (const auto& [_, component_state] : entity_state.components) {
      OX_ASSERT(component_state.buffer.size() < static_cast<usize>(~0_u16));

      packet_size += sizeof(flecs::id_t); //  `id`
      packet_size += sizeof(u64);         // `hash`
      packet_size += sizeof(u16);         // `buffer` size
      packet_size += ox::size_bytes(component_state.buffer);
    }
  }

  auto* packet = new_packet(packet_size, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  buf.write(NetPacketType::SceneSnapshot);
  buf.write<NetSceneSnapshotPacket>({
    .sequence = sequence,
    .entitiy_count = static_cast<u32>(state.entities.size()),
    .removed_entity_count = static_cast<u32>(state.removed_entities.size()),
  });
  // `entities`
  for (const auto& [_, entity_state] : state.entities) {
    buf.write(entity_state.entity_id);                      // `entity_id`
    buf.write<u16>(entity_state.components.size());         // `components` count
    buf.write<u16>(entity_state.removed_components.size()); // `removed_components` count

    // `components`
    for (const auto& [_, component_state] : entity_state.components) {
      OX_ASSERT(component_state.buffer.size() < static_cast<usize>(~0_u16));

      buf.write(component_state.id);            // `id`
      buf.write(component_state.hash);          // `hash`
      buf.write(component_state.buffer.size()); // `buffer` size
      if (!component_state.buffer.empty()) {
        buf.write_span(std::span(component_state.buffer));
      }
    }
  }

  OX_ASSERT(buf.remaining() == 0);

  return NetPacket{.inner = packet};
}

auto NetPacket::client_ack(const NetClientAckPacket& info) -> option<NetPacket> {
  ZoneScoped;

  auto packet_size = 0;
  packet_size += sizeof(NetClientAckPacket::acked);
  auto* packet = new_packet(packet_size, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  buf.write(NetPacketType::ClientAck);
  buf.write(info.acked);

  OX_ASSERT(buf.remaining() == 0);

  return NetPacket{.inner = packet};
}

auto NetPacket::rpc(std::string_view proc, std::span<NetRPCPacket::Parameter> params) -> option<NetPacket> {
  ZoneScoped;

  auto total_parameters_size = 0_u16;
  auto packet_size = 0;
  packet_size += sizeof(NetRPCPacket::proc_hash);
  packet_size += sizeof(NetRPCPacket::parameter_count);
  packet_size += sizeof(NetRPCPacket::total_parameters_size);
  for (const auto& param : params) {
    auto param_size = 0;
    param_size += sizeof(u16);     // `data_size`
    param_size += param.data_size; // `data`
    packet_size += param_size;
    total_parameters_size += param_size;
  }

  auto* packet = new_packet(packet_size, ENET_PACKET_FLAG_RELIABLE);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  buf.write(NetPacketType::RPC);
  buf.write(ankerl::unordered_dense::detail::wyhash::hash(proc.data(), proc.size())); // `proc_hash`
  buf.write<u8>(params.size());                                                       // `parameter_count`
  buf.write(total_parameters_size);                                                   // `total_parameter_size`
  for (const auto& param : params) {
    buf.write(param.data_size);
    buf.write_bytes(param.data, param.data_size);
  }

  return NetPacket{.inner = packet};
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
} // namespace ox
