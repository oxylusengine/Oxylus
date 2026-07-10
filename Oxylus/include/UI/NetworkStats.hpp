#pragma once

#include "Networking/Fwd.hpp"

namespace ox {
class NetworkStatsViewer {
public:
  static auto draw_network_stats(const NetStats& stats, NetClientID client_id) -> void;
};
} // namespace ox
