#pragma once

#include <vuk/Types.hpp>

#include "Utils/Timestep.hpp"

namespace ox {
class Layer {
public:
  Layer(const std::string& name = "Layer");
  virtual ~Layer() = default;

  virtual void on_attach() {}
  virtual void on_detach() {}
  virtual void on_update(const Timestep& delta_time) {}
  virtual void on_render(vuk::Extent3D extent, vuk::Format format) {}

  const std::string& get_name() const { return debug_name; }

protected:
  std::string debug_name;
};
} // namespace ox
