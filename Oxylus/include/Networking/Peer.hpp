#pragma once

#include <string>

#include "Core/Types.hpp"

typedef struct _ENetPeer ENetPeer;

namespace ox {
struct Peer {
  usize id;
  std::string name;
  ENetPeer* peer;

  Peer(usize id_, const std::string& name_, ENetPeer* peer_ = nullptr);
};
} // namespace ox
