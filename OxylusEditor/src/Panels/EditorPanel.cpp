#include "EditorPanel.hpp"

#include <fmt/format.h>

#include "Utils/StringUtils.hpp"
#include "imgui.h"

namespace ox {
uint32_t EditorPanel::_count = 0;

EditorPanel::EditorPanel(const char* name, const char8_t* icon, bool default_show)
    : visible(default_show),
      _name(name),
      _icon(icon) {
  _id = fmt::format(" {} {}\t\t###{}{}", StringUtils::from_char8_t(icon), name, _count, name);
  _count++;
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
} // namespace ox
