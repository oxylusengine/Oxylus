#pragma once

#include <stdint.h>
#include <string>

namespace ox {
class EditorPanel {
public:
  bool visible;

  EditorPanel(const char* name = "Unnamed Panel", const char8_t* icon = u8"", bool default_show = false);
  virtual ~EditorPanel() = default;

  EditorPanel(const EditorPanel& other) = delete;
  EditorPanel(EditorPanel&& other) = delete;
  EditorPanel& operator=(const EditorPanel& other) = delete;
  EditorPanel& operator=(EditorPanel&& other) = delete;

  virtual void on_update() {}

  virtual void on_render(vuk::Extent3D extent, vuk::Format format) = 0;

  const char* get_name() const { return _name.c_str(); }
  const char* get_id() const { return _id.c_str(); }
  const char8_t* get_icon() const { return _icon; }

protected:
  bool on_begin(int32_t window_flags = 0);
  void on_end() const;

  std::string _name;
  const char8_t* _icon;
  std::string _id;

private:
  static uint32_t _count;
};
} // namespace ox
