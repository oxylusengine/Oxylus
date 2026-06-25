#include "EditorPanelState.hpp"

#include <fmt/format.h>

namespace ox {
u32 EditorPanelState::count = 0;

EditorPanelState::EditorPanelState(const char* name_, const char* icon_, bool default_show, bool closable_)
    : visible(default_show),
      name(name_),
      icon(icon_),
      closable(closable_) {
  update_id();
}

auto EditorPanelState::set_name(this EditorPanelState& self, const std::string& name_) -> void {
  self.name = name_;
  self.update_id();
}

bool EditorPanelState::on_begin(this EditorPanelState& self, int32_t window_flags) {
  ImGui::SetNextWindowSize(ImVec2(self.window_default_size.x, self.window_default_size.y), self.window_sizing_cond);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

  if (self.window_center_at_appear) {
    const auto center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, self.window_center_cond, ImVec2(0.5f, 0.5f));
  }

  return ImGui::Begin(
    self.id.c_str(),
    self.closable ? &self.visible : nullptr,
    window_flags | ImGuiWindowFlags_NoCollapse
  );
}

void EditorPanelState::on_end(this const EditorPanelState& self) {
  ImGui::PopStyleVar();
  ImGui::End();
}

void EditorPanelState::update_id(this EditorPanelState& self) {
  self.id = fmt::format(" {} {}\t\t###{}{}", self.icon, self.name, count, self.name);
  count++;
}

} // namespace ox
