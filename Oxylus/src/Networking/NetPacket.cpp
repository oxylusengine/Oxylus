#include "Networking/NetPacket.hpp"

#include <enet.h>
#include <utility>

namespace ox {
auto NetPacket::prepare_raw(NetPacketType type, usize packet_size, const void* initial_data) -> option<NetPacket> {
  ZoneScoped;

  auto* packet = enet_packet_create(nullptr, packet_size + sizeof(NetPacketType), 0);
  if (!packet) {
    return nullopt;
  }

  std::memcpy(packet->data, &type, sizeof(NetPacketType));
  if (initial_data) {
    std::memcpy(packet->data + sizeof(NetPacketType), initial_data, packet_size);
  }

  return NetPacket{.inner = packet};
}

auto NetPacket::destroy(this NetPacket& self) -> void {
  ZoneScoped;

  enet_packet_destroy(self.inner);
}

auto NetPacket::set_flags(this NetPacket& self, NetPacketFlag flags) -> NetPacket& {
  ZoneScoped;

  self.inner->flags = std::to_underlying(flags);

  return self;
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
