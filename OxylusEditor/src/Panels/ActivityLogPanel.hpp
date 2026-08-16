#pragma once

#include <vuk/ImageAttachment.hpp>

#include "Panels/EditorPanelState.hpp"
#include "Utils/Notification.hpp"

namespace ox {
class ActivityLogPanel : public EditorPanelState {
public:
  ActivityLogPanel();

  auto set_system(this ActivityLogPanel& self, NotificationSystem* system) -> void;

  auto on_update(this ActivityLogPanel& self) -> void {}
  auto on_render(this ActivityLogPanel& self, vuk::ImageAttachment swapchain_attachment) -> void;

private:
  ImGuiTextFilter log_filter = {};
  NotificationSystem* notification_system;
};
} // namespace ox
