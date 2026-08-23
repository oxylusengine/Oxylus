#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Scene/Scene.hpp"

namespace ox {
auto RendererInstance::build_light_grid(this RendererInstance& self, LightGridContext& context) -> void {
  ZoneScoped;

  context.grid_origin = self.light_grid_origin;
  context.light_grid_buffer = self.renderer.render_context->alloc_transient_buffer(
    vuk::MemoryUsage::eGPUonly,
    GPU::LIGHT_GRID_CELL_COUNT * sizeof(glm::uvec4)
  );

  const auto shadow_light_count = self.prepared_frame.shadow_point_light_count +
                                  self.prepared_frame.shadow_spot_light_count;
  if (shadow_light_count == 0) {
    auto zero_fill_pass = vuk::make_pass("light grid zero", [](vuk::CommandBuffer& cmd_list, VUK_BA(vuk::eClear) grid) {
      cmd_list.fill_buffer(grid, 0_u32);
      return grid;
    });
    context.light_grid_buffer = zero_fill_pass(std::move(context.light_grid_buffer));
    return;
  }

  auto light_grid_pass = vuk::make_pass(
    "light grid",
    [light_count = static_cast<u32>(self.scene.lights.size()), grid_origin = context.grid_origin](
      vuk::CommandBuffer& cmd_list, //
      VUK_BA(vuk::eComputeRead) lights,
      VUK_BA(vuk::eComputeWrite) light_grid
    ) {
      cmd_list //
        .bind_compute_pipeline("light_grid")
        .bind_buffer(0, 0, lights)
        .bind_buffer(0, 1, light_grid)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(light_count, grid_origin))
        .dispatch_invocations(GPU::LIGHT_GRID_CELL_COUNT);

      return std::make_tuple(lights, light_grid);
    }
  );

  std::tie(self.prepared_frame.lights_buffer, context.light_grid_buffer) = light_grid_pass(
    std::move(self.prepared_frame.lights_buffer),
    std::move(context.light_grid_buffer)
  );
}
} // namespace ox
