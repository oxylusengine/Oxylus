#include "Core/UUID.hpp"
#include "Networking/NetServer.hpp"

struct GameServer : ox::NetServer {
  ox::UUID active_scene = {};
};
