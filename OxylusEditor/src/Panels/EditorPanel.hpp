#pragma once

#include <stdint.h>
#include <string>
#include <vuk/Types.hpp>

namespace ox {
class EditorPanel {
public:
  bool visible;

  EditorPanel(const char* name = "Unnamed Panel", const char* icon = "", bool default_show = false);
  virtual ~EditorPanel() = default;

  EditorPanel(const EditorPanel& other) = delete;
  EditorPanel(EditorPanel&& other) = delete;
  EditorPanel& operator=(const EditorPanel& other) = delete;
  EditorPanel& operator=(EditorPanel&& other) = delete;

  virtual void on_update() {}

  virtual void on_render(vuk::ImageAttachment swapchain_attachment) = 0;

  const char* get_name() const { return _name.c_str(); }
  const char* get_id() const { return _id.c_str(); }
  const char* get_icon() const { return _icon; }

protected:
  bool on_begin(int32_t window_flags = 0);
  void on_end() const;

  std::string _name;
  const char* _icon;
  std::string _id;

private:
  static uint32_t _count;
};
} // namespace ox
