#pragma once

#include "Render/Fwd.hpp"

#include <ankerl/unordered_dense.h>
#include <functional>
#include <memory>
#include <string>
#include <tracy/Tracy.hpp>
#include <typeindex>
#include <vector>

#include "Core/Types.hpp"
#include "Utils/Log.hpp"

namespace ox {
template <typename T>
concept EditorPanel = requires(T& panel, const SwapchainInfo& swapchain_info, const std::string& str, const char* cstr) {
  { panel.visible } -> std::convertible_to<bool>;

  { panel.on_update() } -> std::same_as<void>;
  { panel.on_render(swapchain_info) } -> std::same_as<void>;

  { panel.get_name() } -> std::convertible_to<const char*>;
  { panel.get_id() } -> std::convertible_to<const char*>;
  { panel.get_icon() } -> std::convertible_to<const char*>;
  { panel.set_name(str) } -> std::same_as<void>;
  { panel.set_icon(cstr) } -> std::same_as<void>;
};

struct PanelMetadata {
  bool* visible;
  const char* name;
  const char* icon;
};

class EditorPanelRegistry {
public:
  std::vector<PanelMetadata> panel_metadata = {};

  template <EditorPanel T, typename... Args>
  auto add(Args&&... args) -> T* {
    ZoneScoped;

    auto type_index = std::type_index(typeid(T));
    OX_ASSERT(!registry.contains(type_index));

    auto deleter = [](void* self) {
      delete static_cast<T*>(self);
    };
    auto& panel_wrapper = registry.try_emplace(type_index, PanelPtr(new T(std::forward<Args>(args)...), deleter))
                            .first->second;

    T* panel = static_cast<T*>(panel_wrapper.get());

    panel_metadata.push_back({&panel->visible, panel->get_name(), panel->get_icon()});

    update_callbacks.emplace_back([panel]() {
      if (panel->visible) {
        panel->on_update();
      }
    });

    render_callbacks.emplace_back([panel](const SwapchainInfo& swapchain_info) {
      if (panel->visible) {
        panel->on_render(swapchain_info);
      }
    });

    return panel;
  }

  template <EditorPanel T>
  auto get() -> T& {
    ZoneScoped;
    auto it = registry.find(std::type_index(typeid(T)));
    OX_CHECK_NE(it, registry.end());
    return *static_cast<T*>(it->second.get());
  }

  auto update_all(this EditorPanelRegistry& self) -> void;
  auto render_all(this EditorPanelRegistry& self, const SwapchainInfo& swapchain_info) -> void;

  auto draw_window_menu(this EditorPanelRegistry& self) -> void;

private:
  using PanelPtr = std::unique_ptr<void, void (*)(void*)>;
  using Registry = ankerl::unordered_dense::map<std::type_index, PanelPtr, TypeIndexHash>;

  Registry registry = {};

  std::vector<std::function<void()>> update_callbacks = {};
  std::vector<std::function<void(const SwapchainInfo&)>> render_callbacks = {};
};

} // namespace ox
