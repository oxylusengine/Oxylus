#pragma once

#include <enet.h>

#include "Core/Types.hpp"

namespace ox {
struct Peer {
  usize id;
  std::string name;
  ENetPeer* peer;

  Peer(usize id_, const std::string& name_, ENetPeer* peer_ = nullptr) : id(id_), name(name_), peer(peer_) {}
};
} // namespace ox
