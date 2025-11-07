#include "EditorPanel.hpp"

#include <fmt/format.h>
#include <imgui.h>

namespace ox {
uint32_t EditorPanel::_count = 0;

EditorPanel::EditorPanel(const char* name, const char* icon, bool default_show)
    : visible(default_show),
      _name(name),
      _icon(icon) {
  update_id();
  _count++;
}

auto EditorPanel::set_name(const std::string& name) -> void {
  _name = name;
  update_id();
}

bool EditorPanel::on_begin(int32_t window_flags) {
  if (!visible)
    return false;

  ImGui::SetNextWindowSize(ImVec2(480, 640), ImGuiCond_Once);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

  ImGui::Begin(_id.c_str(), &visible, window_flags | ImGuiWindowFlags_NoCollapse);

  return true;
}

void EditorPanel::on_end() const {
  ImGui::PopStyleVar();
  ImGui::End();
}

void EditorPanel::update_id() { _id = fmt::format(" {} {}\t\t###{}{}", _icon, _name, _count, _name); }

} // namespace ox
