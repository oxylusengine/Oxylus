#include "Networking/NetPacket.hpp"

#include <enet.h>
#include <utility>

#include "Memory/Buffer.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto RPCParameter::as_f32() -> option<const f32> {
  if (type != Type::Float && data_size != 4) {
    return nullopt;
  }

  return *static_cast<const f32*>(data);
}

auto RPCParameter::as_int64() -> option<const i64> {
  if (type != Type::Int64 && data_size != 8) {
    return nullopt;
  }

  return *static_cast<const i64*>(data);
}
auto RPCParameter::as_str() -> std::string_view {
  if (type != Type::String) {
    return {};
  }

  return {static_cast<const c8*>(data), static_cast<usize>(data_size)};
}

auto new_packet(usize packet_size) -> ENetPacket* {
  ZoneScoped;

  return enet_packet_create(nullptr, packet_size + sizeof(NetPacketType), 0);
}

auto NetPacket::handshake(const NetHandshakePacket& info) -> option<NetPacket> {
  ZoneScoped;

  auto packet_size = 0;
  packet_size += sizeof(NetHandshakePacket::version);
  packet_size += sizeof(NetHandshakePacket::net_id);
  auto* packet = new_packet(packet_size);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  buf.write(NetPacketType::Handshake);
  buf.write(info.version);
  buf.write(info.net_id);

  OX_ASSERT(buf.remaining() == 0);

  return NetPacket{.type = NetPacketType::Handshake, .inner = packet};
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
    packet_size += entity_state.removed_components.size() * sizeof(flecs::id_t);

    // `components`
    for (const auto& [_, component_state] : entity_state.components) {
      OX_ASSERT(component_state.buffer.size() < static_cast<usize>(~0_u16));

      packet_size += sizeof(flecs::id_t); //  `id`
      packet_size += sizeof(u64);         // `hash`
      packet_size += sizeof(u16);         // `buffer` size
      packet_size += ox::size_bytes(component_state.buffer);
    }
  }

  auto* packet = new_packet(packet_size);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  buf.write(NetPacketType::SceneSnapshot);
  buf.write(sequence);
  buf.write(static_cast<u32>(state.entities.size()));
  buf.write(static_cast<u32>(state.removed_entities.size()));

  // `entities`
  for (const auto& [_, entity_state] : state.entities) {
    buf.write(entity_state.entity_id);                      // `entity_id`
    buf.write<u16>(entity_state.components.size());         // `components` count
    buf.write<u16>(entity_state.removed_components.size()); // `removed_components` count

    // `components`
    for (const auto& [_, component_state] : entity_state.components) {
      OX_ASSERT(component_state.buffer.size() < static_cast<usize>(~0_u16));

      buf.write(component_state.id);                 // `id`
      buf.write(component_state.hash);               // `hash`
      buf.write<u16>(component_state.buffer.size()); // `buffer` size
      if (!component_state.buffer.empty()) {
        buf.write_span(std::span(component_state.buffer));
      }
    }

    for (const auto& removed_component_id : entity_state.removed_components) {
      buf.write(removed_component_id);
    }
  }

  for (const auto& removed_entity_id : state.removed_entities) {
    buf.write(removed_entity_id);
  }

  OX_ASSERT(buf.remaining() == 0);

  return NetPacket{.type = NetPacketType::SceneSnapshot, .inner = packet};
}

auto NetPacket::client_ack(const NetClientAckPacket& info) -> option<NetPacket> {
  ZoneScoped;

  auto packet_size = 0;
  packet_size += sizeof(NetClientAckPacket::acked);
  auto* packet = new_packet(packet_size);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  buf.write(NetPacketType::ClientAck);
  buf.write(info.acked);

  OX_ASSERT(buf.remaining() == 0);

  return NetPacket{.type = NetPacketType::ClientAck, .inner = packet};
}

auto NetPacket::rpc(std::string_view proc, std::span<RPCParameter> params) -> option<NetPacket> {
  ZoneScoped;

  auto packet_size = 0;
  packet_size += sizeof(NetRPCPacket::proc_hash);
  packet_size += sizeof(u8);
  for (const auto& param : params) {
    packet_size += sizeof(RPCParameter::Type);
    switch (param.type) {
      case RPCParameter::Type::None: {
      } break;
      case RPCParameter::Type::Byte: {
        packet_size += sizeof(u8);
      } break;
      case RPCParameter::Type::Short: {
        packet_size += sizeof(u16);
      } break;
      case RPCParameter::Type::Float:
      case RPCParameter::Type::Int  : {
        packet_size += sizeof(u32);
      } break;
      case RPCParameter::Type::Double:
      case RPCParameter::Type::Int64 : {
        packet_size += sizeof(u64);
      } break;
      case RPCParameter::Type::UUID: {
        packet_size += sizeof(u64) * 2;
      } break;
      case RPCParameter::Type::String:
      case RPCParameter::Type::ByteArray: {
        packet_size += sizeof(u32);
        packet_size += param.data_size;
      } break;
    }
  }

  auto* packet = new_packet(packet_size);
  if (!packet) {
    return nullopt;
  }

  auto buf = BufferWriter(packet->data, packet->dataLength);
  OX_ASSERT(buf.write(NetPacketType::RPC));
  OX_ASSERT(buf.write(ankerl::unordered_dense::detail::wyhash::hash(proc.data(), proc.size()))); // `proc_hash`
  OX_ASSERT(buf.write<u8>(params.size()));                                                       // `parameter_count`
  for (const auto& param : params) {
    OX_ASSERT(buf.write(param.type));
    switch (param.type) {
      case RPCParameter::Type::None: {
      } break;
      case RPCParameter::Type::Byte: {
        OX_ASSERT(buf.write_bytes(param.data, 1));
      } break;
      case RPCParameter::Type::Short: {
        OX_ASSERT(buf.write_bytes(param.data, 2));
      } break;
      case RPCParameter::Type::Float:
      case RPCParameter::Type::Int  : {
        OX_ASSERT(buf.write_bytes(param.data, 4));
      } break;
      case RPCParameter::Type::Double:
      case RPCParameter::Type::Int64 : {
        OX_ASSERT(buf.write_bytes(param.data, 8));
      } break;
      case RPCParameter::Type::UUID: {
        OX_ASSERT(buf.write_bytes(param.data, 16));
      } break;
      case RPCParameter::Type::String:
      case RPCParameter::Type::ByteArray: {
        OX_ASSERT(buf.write(param.data_size));
        OX_ASSERT(buf.write_bytes(param.data, param.data_size));
      } break;
    }
  }

  return NetPacket{.type = NetPacketType::RPC, .inner = packet};
}

auto NetPacket::from_packet(ENetPacket* packet) -> option<NetPacket> {
  ZoneScoped;

  auto buf = BufferReader(packet->data, packet->dataLength);
  auto packet_type = buf.read<NetPacketType>();
  if (!packet_type.has_value()) {
    return nullopt;
  }

  return NetPacket{.type = packet_type.value(), .inner = packet};
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

auto NetPacket::reader(this NetPacket& self) -> BufferReader {
  ZoneScoped;

  auto buf = BufferReader(self.inner->data, self.inner->dataLength);
  OX_ASSERT(buf.skip(sizeof(NetPacketType)));

  return buf;
}

auto NetPacket::writer(this NetPacket& self) -> BufferWriter {
  ZoneScoped;

  auto buf = BufferWriter(self.inner->data, self.inner->dataLength);
  OX_ASSERT(buf.skip(sizeof(NetPacketType)));

  return buf;
}

auto NetPacket::get_handshake(this NetPacket& self) -> option<NetHandshakePacket> {
  ZoneScoped;

  if (self.type != NetPacketType::Handshake) {
    return nullopt;
  }

  auto buf = self.reader();
  auto version = buf.read<u32>();
  auto net_id = buf.read<u64>();
  if (!version.has_value()) {
    return nullopt;
  }

  return NetHandshakePacket{.version = version.value(), .net_id = net_id.value_or(~0_u64)};
}

auto NetPacket::get_scene_snapshot(this NetPacket& self) -> option<std::pair<u8, SceneState>> {
  ZoneScoped;

  if (self.type != NetPacketType::SceneSnapshot) {
    return nullopt;
  }

  auto buf = self.reader();
  auto sequence = buf.read<u8>();
  if (!sequence.has_value()) {
    return nullopt;
  }

  auto state = SceneState{};
  auto entity_count = buf.read<u32>().value_or(0_u32);
  auto removed_entity_count = buf.read<u32>().value_or(0_u32);

  for (auto entity_idx = 0_u32; entity_idx < entity_count; entity_idx++) {
    auto entity_state = EntityState{};
    auto entity_id = buf.read<flecs::entity_t>();
    auto component_count = buf.read<u16>();
    auto removed_component_count = buf.read<u16>();
    if (!entity_id.has_value() || !component_count.has_value() || !removed_component_count.has_value()) {
      return nullopt;
    }

    entity_state.entity_id = entity_id.value();

    for (auto component_idx = 0_u32; component_idx < component_count.value(); component_idx++) {
      auto component_state = ComponentState{};
      auto component_id = buf.read<flecs::id_t>();
      auto component_hash = buf.read<u64>();
      if (!component_id.has_value() || !component_hash.has_value()) {
        return nullopt;
      }

      component_state.id = component_id.value();
      component_state.hash = component_hash.value();

      auto component_data_size = buf.read<u16>().value_or(0);
      if (component_data_size > 0) {
        component_state.buffer.resize(component_data_size);
        OX_ASSERT(buf.read_bytes(component_state.buffer.data(), component_data_size));
      }

      entity_state.components.emplace(component_id.value(), std::move(component_state));
    }

    for (auto component_idx = 0_u32; component_idx < removed_component_count.value(); component_idx++) {
      auto removed_component_id = buf.read<flecs::id_t>();
      if (!removed_component_id.has_value()) {
        return nullopt;
      }

      entity_state.removed_components.emplace(removed_component_id.value());
    }

    state.entities.emplace(entity_id.value(), std::move(entity_state));
  }

  for (auto entity_idx = 0_u32; entity_idx < removed_entity_count; entity_idx++) {
    auto removed_entity_id = buf.read<flecs::entity_t>();
    if (!removed_entity_id.has_value()) {
      return nullopt;
    }

    state.removed_entities.emplace(removed_entity_id.value());
  }

  return std::pair{sequence.value(), std::move(state)};
}

auto NetPacket::get_client_ack(this NetPacket& self) -> option<NetClientAckPacket> {
  ZoneScoped;

  if (self.type != NetPacketType::ClientAck) {
    return nullopt;
  }

  auto buf = self.reader();
  auto acked = buf.read<u8>();
  if (!acked) {
    return nullopt;
  }

  return NetClientAckPacket{.acked = acked.value()};
}

auto NetPacket::get_rpc(this NetPacket& self) -> option<NetRPCPacket> {
  ZoneScoped;

  if (self.type != NetPacketType::RPC) {
    return nullopt;
  }

  auto buf = self.reader();
  auto proc_hash = buf.read<u64>();
  auto parameter_count = buf.read<u8>();
  if (!proc_hash.has_value() || !parameter_count.has_value()) {
    return nullopt;
  }

  auto params = ankerl::svector<RPCParameter, 8>(parameter_count.value());
  for (auto& param : params) {
    auto param_type = buf.read<RPCParameter::Type>();
    if (!param_type.has_value()) {
      return nullopt;
    }

    param.type = param_type.value();
    switch (param.type) {
      case RPCParameter::Type::None: {
        param.data_size = 0;
      } break;
      case RPCParameter::Type::Byte: {
        param.data_size = 1;
      } break;
      case RPCParameter::Type::Short: {
        param.data_size = 2;
      } break;
      case RPCParameter::Type::Int:
      case RPCParameter::Type::Float: {
        param.data_size = 4;
      } break;
      case RPCParameter::Type::Int64:
      case RPCParameter::Type::Double: {
        param.data_size = 8;
      } break;
      case RPCParameter::Type::UUID: {
        param.data_size = 16;
      } break;
      case RPCParameter::Type::String:
      case RPCParameter::Type::ByteArray: {
        auto param_data_size = buf.read<u32>();
        if (!param_data_size.has_value()) {
          return nullopt;
        }

        param.data_size = param_data_size.value();
      } break;
    }

    param.data = reinterpret_cast<const void*>(buf.data() + buf.offset);
    if (!buf.skip(param.data_size)) {
      return nullopt;
    }
  }

  return NetRPCPacket{.proc_hash = proc_hash.value(), .parameters = std::move(params)};
}
} // namespace ox
