#pragma once

#include <stdint.h>
#include <string>
#include <vuk/Types.hpp>

namespace ox {
class EditorPanel {
public:
  bool visible;

  EditorPanel(
    const char* name = "Unnamed Panel", const char* icon = "", bool default_show = false, bool closable = true
  );
  virtual ~EditorPanel() = default;

  EditorPanel(const EditorPanel& other) = delete;
  EditorPanel(EditorPanel&& other) = delete;
  EditorPanel& operator=(const EditorPanel& other) = delete;
  EditorPanel& operator=(EditorPanel&& other) = delete;

  virtual auto on_update() -> void {}

  virtual auto on_render(vuk::ImageAttachment swapchain_attachment) -> void = 0;

  auto set_name(const std::string& name) -> void;
  auto get_name() const -> const char* { return name_.c_str(); }
  auto get_id() const -> const char* { return id_.c_str(); }

  auto get_icon() const -> const char* { return icon_; }
  auto set_icon(const char* icon) -> void { icon_ = icon; }

protected:
  auto on_begin(int32_t window_flags = 0) -> bool;
  auto on_end() const -> void;

  auto update_id() -> void;

  std::string name_;
  const char* icon_;
  std::string id_;
  bool closable_;

private:
  static uint32_t _count;
};
} // namespace ox
