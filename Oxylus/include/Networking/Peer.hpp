#pragma once

#include <enet.h>

#include "Core/Types.hpp"

namespace ox {
struct Peer {
  u32 id;
  std::string name;
  ENetPeer* peer;

  Peer(u32 id, const std::string& name, ENetPeer* peer = nullptr) : id(id), name(name), peer(peer) {}
};
} // namespace ox
