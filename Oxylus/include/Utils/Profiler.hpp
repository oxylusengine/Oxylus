#pragma once

#include <memory>

#include "Render/Graph/Fwd.hpp"

namespace ox {
class RenderContext;
struct TracyProfilerState;

class TracyProfiler {
public:
  std::unique_ptr<TracyProfilerState> state = {};

  TracyProfiler();
  ~TracyProfiler();
  auto init_for_vulkan(this TracyProfiler& self, RenderContext& context) -> void;
  auto callbacks(this TracyProfiler& self) -> vir::RenderGraphCallbacks;
  auto destroy_context(this TracyProfiler& self) -> void;
};
} // namespace ox
