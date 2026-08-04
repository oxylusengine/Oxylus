#include "UI/NetStatsViewer.hpp"

#include <imgui.h>

#include "Memory/Stack.hpp"

namespace ox {
auto draw_stats_rows(memory::ScopedStack& stack, const NetStats& stats) -> void {
  ImGui::TextUnformatted(stack.format_char("ping: {}", stats.ping));
  ImGui::TextUnformatted(stack.format_char("sent_bytes: {}", stats.sent_bytes));
  ImGui::TextUnformatted(stack.format_char("received_bytes: {}", stats.received_bytes));
  ImGui::TextUnformatted(stack.format_char("sent_packets: {}", stats.sent_packets));
  ImGui::TextUnformatted(stack.format_char("packets_lost: {}", stats.packets_lost));
  ImGui::TextUnformatted(stack.format_char("rtt: {}", stats.rtt));
  ImGui::TextUnformatted(stack.format_char("last_sent_bytes: {}", stats.last_sent_bytes));
  ImGui::TextUnformatted(stack.format_char("last_received_bytes: {}", stats.last_received_bytes));
  ImGui::TextUnformatted(stack.format_char("last_sent_packets: {}", stats.last_sent_packets));
}

auto NetStatsViewer::draw_network_stats(const NetClient& client) -> void {
  ZoneScoped;

  memory::ScopedStack stack;

  if (ImGui::Begin("NetStats")) {
    ImGui::TextUnformatted(stack.format_char("client_id: {}", client.net_id));
    draw_stats_rows(stack, client.stats);
  }
  ImGui::End();
}

auto NetStatsViewer::draw_network_stats(NetServer& server) -> void {
  ZoneScoped;

  memory::ScopedStack stack;

  if (ImGui::Begin("NetStats")) {
    const auto client_ids = server.client_ids();
    ImGui::TextUnformatted(stack.format_char("connected clients: {}", client_ids.size()));

    for (const auto client_id : client_ids) {
      const auto* client = server.client(client_id);
      if (!client) {
        continue;
      }

      ImGui::SeparatorText(stack.format_char("client_id: {}", client->net_id));
      draw_stats_rows(stack, client->stats);
    }
  }
  ImGui::End();
}
} // namespace ox
