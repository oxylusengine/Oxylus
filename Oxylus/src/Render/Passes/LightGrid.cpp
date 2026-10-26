#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/PushConstants.hpp"
#include "Scene/Scene.hpp"

namespace ox {
using LightGridPush = PushConstants<u32, glm::vec3>;

constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

auto RendererInstance::bind_light_grid_variables(
  this const RendererInstance& self, vir::Program& program, const LightGridVariables& variables
) -> void {
  if (!program.has_variable(variables.push)) {
    return;
  }
  program.set_bytes(variables.push, LightGridPush(static_cast<u32>(self.scene.lights.size()), self.light_grid_origin));
}

auto RendererInstance::build_light_grid(
  vir::Module& module, RenderContext& render_context, FrameRecording& recording, LightGridContext& context
) -> void {
  ZoneScoped;
  auto& prepared = recording.prepared;
  auto& values = recording.values;
  const auto& shape = recording.shape;

  context.light_grid_buffer = module.transient_buffer({
    .size = GPU::LIGHT_GRID_CELL_COUNT * sizeof(glm::uvec4),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "light grid",
  });

  if (!shape.has_pointspot_shadows) {
    context.light_grid_buffer = module.fill_buffer(context.light_grid_buffer, 0);
    return;
  }

  auto& variables = prepared.light_grid;
  variables.push = module.declare_bytes_var("light grid push", sizeof(LightGridPush));

  std::tie(values.lights, context.light_grid_buffer) = std::tuple_cat(
    module
      .begin_compute(
        {{values.lights, vir::Access::ComputeRead}, {context.light_grid_buffer, vir::Access::ComputeWrite}}
      )
      .with_name("light grid")
      .bind_compute_pipeline(render_context.pipeline("light_grid"))
      .bind_buffer(0, 0, values.lights)
      .bind_buffer(0, 1, context.light_grid_buffer)
      .push_constants_from(prepared.light_grid.push)
      .dispatch_invocations(GPU::LIGHT_GRID_CELL_COUNT, 1, 1)
      .end_compute()
  );
}
} // namespace ox
