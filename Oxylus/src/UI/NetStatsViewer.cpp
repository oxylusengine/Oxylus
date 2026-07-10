#include "UI/NetStatsViewer.hpp"

#include <imgui.h>

#include "Memory/Stack.hpp"

namespace ox {
auto NetStatsViewer::draw_network_stats(const NetClient& client) -> void {
  ZoneScoped;

  memory::ScopedStack stack;

  if (ImGui::Begin("NetStats")) {
    ImGui::TextUnformatted(stack.format_char("client_id: {}", client.net_id));
    ImGui::TextUnformatted(stack.format_char("ping: {}", client.stats.ping));
    ImGui::TextUnformatted(stack.format_char("sent_bytes: {}", client.stats.sent_bytes));
    ImGui::TextUnformatted(stack.format_char("received_bytes: {}", client.stats.received_bytes));
    ImGui::TextUnformatted(stack.format_char("sent_packets: {}", client.stats.sent_packets));
    ImGui::TextUnformatted(stack.format_char("packets_lost: {}", client.stats.packets_lost));
    ImGui::TextUnformatted(stack.format_char("rtt: {}", client.stats.rtt));
    ImGui::TextUnformatted(stack.format_char("last_sent_bytes: {}", client.stats.last_sent_bytes));
    ImGui::TextUnformatted(stack.format_char("last_received_bytes: {}", client.stats.last_received_bytes));
    ImGui::TextUnformatted(stack.format_char("last_sent_packets: {}", client.stats.last_sent_packets));
  }
  ImGui::End();
}
} // namespace ox
