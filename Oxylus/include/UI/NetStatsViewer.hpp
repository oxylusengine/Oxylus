#pragma once

#include "Networking/NetClient.hpp"

namespace ox {
class NetStatsViewer {
public:
  static auto draw_network_stats(const NetClient& client) -> void;
};
} // namespace ox
