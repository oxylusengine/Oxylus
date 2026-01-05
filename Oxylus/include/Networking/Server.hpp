#pragma once

#include <ankerl/unordered_dense.h>
#include <enet.h>
#include <flecs.h>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct NetServer {
  ENetHost* host = nullptr;
  ankerl::unordered_dense::map<ENetPeer*, flecs::entity> peer_to_entity = {};

  static auto create(u16 port, u32 max_clients) -> option<NetServer>;
  auto destroy(this NetServer&) -> void;
};
} // namespace ox
