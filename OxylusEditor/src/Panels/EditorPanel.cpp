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
  _count++;
}

auto EditorPanel::set_name(const std::string& name) -> void {
  name_ = name;
  update_id();
}

bool EditorPanel::on_begin(int32_t window_flags) {
  if (!visible)
    return false;

  ImGui::SetNextWindowSize(ImVec2(480, 640), ImGuiCond_Once);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

  return ImGui::Begin(id_.c_str(), closable_ ? &visible : nullptr, window_flags | ImGuiWindowFlags_NoCollapse);
}

void EditorPanel::on_end() const {
  if (visible) {
    ImGui::PopStyleVar();
    ImGui::End();
  }
}

void EditorPanel::update_id() { id_ = fmt::format(" {} {}\t\t###{}{}", icon_, name_, _count, name_); }

} // namespace ox
