#include "Networking/Peer.hpp"

#include <enet.h>

namespace ox {
Peer::Peer(usize id_, const std::string& name_, ENetPeer* peer_) : id(id_), name(name_), peer(peer_) {}
} // namespace ox
