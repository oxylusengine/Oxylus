#include <vir/RenderGraph.hpp>
#include <vir/Types.hpp>

#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"

namespace ox {
constexpr static auto TRANSIENT_BUFFER_USAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

static_assert(GPU::DECAL_TILE_SIZE * GPU::DECAL_TILE_SIZE == GPU::MAX_DECALS);

auto RendererInstance::cull_decals(
  this RendererInstance& self, vir::Module& module, FrameRecording& recording, DecalCullContext& context
) -> void {
  ZoneScoped;
  auto& values = recording.values;
  auto& render_context = *self.renderer.render_context;

  const auto tile_count_x = (recording.shape.render_size.x + GPU::DECAL_TILE_SIZE - 1) / GPU::DECAL_TILE_SIZE;
  const auto tile_count_y = (recording.shape.render_size.y + GPU::DECAL_TILE_SIZE - 1) / GPU::DECAL_TILE_SIZE;
  values.decal_tiles = module.transient_buffer({
    .size = static_cast<u64>(tile_count_x) * tile_count_y * GPU::DECAL_TILE_WORDS * sizeof(u32),
    .usage = TRANSIENT_BUFFER_USAGE,
    .location = vir::MemoryLocation::GpuOnly,
    .name = "decal tiles",
  });

  std::tie(values.camera, values.decals, context.depth_attachment, values.decal_tiles) = std::tuple_cat(
    module
      .begin_compute(
        {{values.camera, vir::Access::ComputeUniformRead},
         {values.decals, vir::Access::ComputeRead},
         {context.depth_attachment, vir::Access::ComputeSampled},
         {values.decal_tiles, vir::Access::ComputeWrite}}
      )
      .with_name("decal cull")
      .bind_compute_pipeline(render_context.pipeline("decal_cull"))
      .bind_buffer(0, 0, values.camera)
      .bind_image(0, 1, context.depth_attachment)
      .bind_buffer(0, 2, values.decal_tiles)
      .push_constant_address(0, values.decals)
      .push_constants_from_at(8, recording.prepared.decal_count)
      .dispatch(tile_count_x, tile_count_y, 1)
      .end_compute()
  );
}
} // namespace ox
