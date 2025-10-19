#pragma once

#include <vuk/Types.hpp>

#include "Utils/Timestep.hpp"

namespace ox {
class Layer {
public:
  Layer(const std::string& name = "Layer");
  virtual ~Layer() = default;

  virtual auto on_attach() -> void {}
  virtual auto on_detach() -> void {}
  virtual auto on_update(const Timestep& delta_time) -> void {}
  virtual auto on_render(vuk::Extent3D extent, vuk::Format format) -> void {}

  auto get_name() const -> const std::string& { return debug_name; }

protected:
  std::string debug_name;
};
} // namespace ox
