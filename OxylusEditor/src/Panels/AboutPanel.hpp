#pragma once

#include "Panels/EditorPanelState.hpp"
#include "Render/Fwd.hpp"

namespace ox {
class AboutPanel : public EditorPanelState {
public:
  AboutPanel();

  auto on_update(this AboutPanel& self) -> void {}
  auto on_render(this AboutPanel& self, const SwapchainInfo& swapchain_info) -> void;

private:
  f64 copied_until = 0.0;
};
} // namespace ox
