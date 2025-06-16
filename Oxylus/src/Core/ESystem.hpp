#pragma once

#include <vuk/Types.hpp>

namespace ox {
class App;

/// Engine systems interface
class ESystem {
public:
  App* app = nullptr;

  ESystem() = default;
  virtual ~ESystem() = default;
  DELETE_DEFAULT_CONSTRUCTORS(ESystem)

  virtual auto init() -> std::expected<void, std::string> = 0;
  virtual auto deinit() -> std::expected<void, std::string> = 0;

  virtual auto on_update() -> void {}
  virtual auto on_render(vuk::Extent3D extent, vuk::Format format) -> void {}
};
} // namespace ox
