#pragma once

#include "Panels/EditorPanelState.hpp"

namespace vuk {
struct ImageAttachment;
}

namespace ox {
class AboutPanel : public EditorPanelState {
public:
  AboutPanel();

  auto on_update(this AboutPanel& self) -> void {}
  auto on_render(this AboutPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  f64 copied_until = 0.0;
};
} // namespace ox
