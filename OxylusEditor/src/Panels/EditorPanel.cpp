#include "EditorPanel.hpp"

#include <fmt/format.h>
#include <imgui.h>

namespace ox {
uint32_t EditorPanel::_count = 0;

EditorPanel::EditorPanel(const char* name, const char* icon, bool default_show, bool closable)
    : visible(default_show),
      name_(name),
      icon_(icon),
      closable_(closable) {
  update_id();
}

auto EditorPanel::set_name(const std::string& name) -> void {
  name_ = name;
  update_id();
}

bool EditorPanel::on_begin(int32_t window_flags) {
  ImGui::SetNextWindowSize(ImVec2(this->window_default_size.x, this->window_default_size.y), this->window_sizing_cond);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

  if (this->window_center_at_appear) {
    const auto center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, this->window_center_cond, ImVec2(0.5f, 0.5f));
  }

  return ImGui::Begin(id_.c_str(), closable_ ? &visible : nullptr, window_flags | ImGuiWindowFlags_NoCollapse);
}

void EditorPanel::on_end() const {
  ImGui::PopStyleVar();
  ImGui::End();
}

void EditorPanel::update_id() {
  id_ = fmt::format(" {} {}\t\t###{}{}", icon_, name_, _count, name_);
  _count++;
}

} // namespace ox
