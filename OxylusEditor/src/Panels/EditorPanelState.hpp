#pragma once

#include <imgui.h>
#include <stdint.h>
#include <string>

#include "Core/Types.hpp"

namespace ox {
class EditorPanelState {
public:
  bool visible;

  EditorPanelState(
    const char* name_ = "Unnamed Panel", const char* icon_ = "", bool default_show = false, bool closable_ = true
  );

  ~EditorPanelState() = default;

  EditorPanelState(const EditorPanelState& other) = delete;
  EditorPanelState(EditorPanelState&& other) = delete;
  EditorPanelState& operator=(const EditorPanelState& other) = delete;
  EditorPanelState& operator=(EditorPanelState&& other) = delete;

  auto set_name(this EditorPanelState& self, const std::string& name_) -> void;
  auto get_name(this const EditorPanelState& self) -> const char* { return self.name.c_str(); }
  auto get_id() const -> const char* { return id.c_str(); }

  auto get_icon() const -> const char* { return icon; }
  auto set_icon(const char* p_icon) -> void { icon = p_icon; }

protected:
  std::string name;
  const char* icon;
  std::string id;
  bool closable;
  ImVec2 window_default_size = {480, 640};
  ImGuiCond window_sizing_cond = ImGuiCond_Once;
  bool window_center_at_appear = false;
  ImGuiCond window_center_cond = ImGuiCond_Once;

  auto on_begin(this EditorPanelState& self, i32 window_flags = 0) -> bool;
  auto on_end(this const EditorPanelState& self) -> void;
  auto update_id(this EditorPanelState& self) -> void;

private:
  static u32 count;
};

} // namespace ox
