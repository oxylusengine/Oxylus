#pragma once

#include "Core/Types.hpp"

namespace ox {
class RendererInstance;

// Adds the editor grid to the next render performed by this renderer instance.
auto add_editor_grid_stage(RendererInstance& renderer_instance, f32 grid_distance) -> void;
} // namespace ox
