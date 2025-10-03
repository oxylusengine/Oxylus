#pragma once

#include <enet.h>
#include <expected>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct Packet {
  u32 packet_id;
  std::vector<u8> data;

  Packet(u32 id) : packet_id(id) {}
  Packet(u32 id, const std::vector<u8>& message) : packet_id(id), data(message) {}

  static auto parse_packet(ENetPacket* enet_packet) -> option<Packet> {
    ZoneScoped;

    // Packet only needs packet_id (4 bytes)
    if (enet_packet->dataLength < sizeof(u32)) {
      return nullopt;
    }

    const uint8_t* buffer = enet_packet->data;
    u32 packet_id;

    std::memcpy(&packet_id, buffer, sizeof(u32));
    buffer += sizeof(u32);

    std::vector<u8> packet_data = {};
    usize remaining_data = enet_packet->dataLength - sizeof(u32);
    if (remaining_data > 0) {
      packet_data.resize(remaining_data);
      std::memcpy(packet_data.data(), buffer, remaining_data);
    }

    return Packet(packet_id, packet_data);
  }

  auto size(this const Packet& self) -> usize { return sizeof(u32) + sizeof(u32) + (self.data.size() * sizeof(u8)); }

  auto clear_data(this Packet& self) -> Packet& {
    self.data.clear();
    return self;
  }

  auto serialize(this const Packet& self) -> std::vector<u8> {
    ZoneScoped;

    std::vector<u8> buffer = {};
    buffer.reserve(self.size());

    const u8* packet_id_bytes = reinterpret_cast<const u8*>(&self.packet_id);
    buffer.insert(buffer.end(), packet_id_bytes, packet_id_bytes + sizeof(u32));

    buffer.insert(buffer.end(), self.data.begin(), self.data.end());

    return buffer;
  }

  template <typename T>
  auto add(this Packet& self, const T& value) -> Packet& {
    ZoneScoped;
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "T must be an arithmetic type or enum");
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    self.data.insert(self.data.end(), bytes, bytes + sizeof(T));
    return self;
  }

  auto add_string(this Packet& self, const std::string& str) -> Packet& {
    ZoneScoped;
    // length first, data second
    self.add<u32>(static_cast<u32>(str.size()));
    self.data.insert(self.data.end(), str.begin(), str.end());
    return self;
  }

  auto add_bytes(this Packet& self, const std::vector<u8>& bytes) -> Packet& {
    ZoneScoped;
    // length first, data second
    self.add<u32>(static_cast<u32>(bytes.size()));
    self.data.insert(self.data.end(), bytes.begin(), bytes.end());
    return self;
  }

  auto add_bytes(this Packet& self, const u8* data, usize size) -> Packet& {
    ZoneScoped;
    // length first, data second
    self.add<u32>(static_cast<u32>(size));
    self.data.insert(self.data.end(), data, data + size);
    return self;
  }

  template <typename T>
  auto read(this const Packet& self, usize& offset) -> std::expected<T, std::string> {
    ZoneScoped;
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "T must be an arithmetic type or enum");

    if (offset + sizeof(T) > self.data.size()) {
      return std::unexpected("Read beyond packet data");
    }

    T value;
    std::memcpy(&value, self.data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
  }

  auto read_string(this const Packet& self, usize& offset) -> std::expected<std::string, std::string> {
    ZoneScoped;
    auto length_result = self.read<u32>(offset);
    if (!length_result) {
      return std::unexpected(length_result.error());
    }

    u32 length = *length_result;
    if (offset + length > self.data.size()) {
      return std::unexpected("String length exceeds packet data");
    }

    std::string str(reinterpret_cast<const char*>(self.data.data() + offset), length);
    offset += length;
    return str;
  }

  auto read_bytes(this const Packet& self, usize& offset) -> std::expected<std::vector<u8>, std::string> {
    ZoneScoped;
    auto length_result = self.read<u32>(offset);
    if (!length_result) {
      return std::unexpected(length_result.error());
    }

    u32 length = *length_result;
    if (offset + length > self.data.size()) {
      return std::unexpected("Bytes length exceeds packet data");
    }

    std::vector<u8> bytes(self.data.begin() + offset, self.data.begin() + offset + length);
    offset += length;
    return bytes;
  }
};
} // namespace ox
