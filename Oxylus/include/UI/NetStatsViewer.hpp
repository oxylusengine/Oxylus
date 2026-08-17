#pragma once

#include "Networking/NetClient.hpp"
#include "Networking/NetServer.hpp"

namespace ox {
class NetStatsViewer {
public:
  static auto draw_network_stats(const NetClient& client) -> void;
  // A server has no link of its own, so this draws one section per connected client instead.
  static auto draw_network_stats(NetServer& server) -> void;
};
} // namespace ox
