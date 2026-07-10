#include "UI/NetworkStats.hpp"

#include <imgui.h>

#include "Memory/Stack.hpp"

namespace ox {
auto NetworkStatsViewer::draw_network_stats(const NetStats& stats, NetClientID client_id) -> void {
  ZoneScoped;

  memory::ScopedStack stack;

  if (ImGui::Begin("NetStats")) {
    ImGui::TextUnformatted(stack.format_char("client_id: {}", static_cast<u64>(client_id)));
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
  ImGui::End();
}
} // namespace ox
