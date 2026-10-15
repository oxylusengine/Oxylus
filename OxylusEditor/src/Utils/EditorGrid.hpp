#pragma once

#include <functional>

#include "Core/Types.hpp"

namespace ox {
class RendererInstance;

// Adds the editor grid to the next render performed by this renderer instance. The program keeps the
// stage, so the distance is read every frame rather than captured.
auto add_editor_grid_stage(RendererInstance& renderer_instance, std::function<auto()->f32> grid_distance) -> void;
} // namespace ox
