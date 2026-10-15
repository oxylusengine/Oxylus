#include "Scene/Terrain.hpp"

#include <algorithm>
#include <bit>

#include "Render/Graph/Allocator.hpp"
#include "Render/Graph/Module.hpp"
#include "Render/Graph/Types.hpp"
#include "Render/RenderContext.hpp"
#include "Utils/Log.hpp"

namespace ox {
static_assert(sizeof(GPU::TerrainGenerate) <= 128, "push constant blocks must fit the guaranteed 128 bytes");
static_assert(sizeof(GPU::TerrainBrushParams) <= 128, "push constant blocks must fit the guaranteed 128 bytes");

constexpr auto TERRAIN_MAP_SAMPLER = vir::SamplerInfo{};

auto create_terrain_edit_maps(Terrain& terrain, const vir::Extent3D& extent) -> void {
  terrain.height_edit = Texture::create(
    {.format = vir::Format::R32Sfloat,
     .extent = extent,
     .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::TransferSrc,
     .sampler_info = TERRAIN_MAP_SAMPLER}
  );
  terrain.splat_edit = Texture::create(
    {.format = vir::Format::RGBA8Unorm,
     .extent = extent,
     .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::TransferSrc,
     .sampler_info = TERRAIN_MAP_SAMPLER}
  );

  terrain.height_edit.set_name("terrain height edit");
  terrain.splat_edit.set_name("terrain splat edit");
}

auto terrain_collision_sample_count(u32 requested) -> u32 {
  return std::bit_ceil(std::clamp(requested, TERRAIN_COLLISION_MIN_SAMPLES, TERRAIN_COLLISION_MAX_SAMPLES));
}

template <typename PushF>
static auto record_generate(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  PushF&& push,
  vir::Count groups_x,
  vir::Count groups_y
) -> void {
  ZoneScoped;

  module
    .begin_compute(
      {{maps.heightmap, vir::Access::ComputeRW},
       {maps.ridgemap, vir::Access::ComputeRW},
       {maps.height_edit, vir::Access::ComputeSampled},
       {maps.region, vir::Access::ComputeRead}}
    )
    .with_name("terrain generate")
    .bind_compute_pipeline(render_context.pipeline("terrain_generate"))
    .bind_image(0, 0, maps.heightmap)
    .bind_image(0, 1, maps.ridgemap)
    .bind_image(0, 2, maps.height_edit)
    .bind_buffer(0, 3, maps.region);
  push(module);
  module.dispatch(groups_x, groups_y, 1);
  std::tie(maps.heightmap, maps.ridgemap, maps.height_edit, maps.region) = std::tuple_cat(module.end_compute<4>());
}

template <typename PushF>
static auto record_derive(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  PushF&& push,
  vir::Count groups_x,
  vir::Count groups_y
) -> void {
  ZoneScoped;

  module
    .begin_compute(
      {{maps.heightmap, vir::Access::ComputeSampled},
       {maps.ridgemap, vir::Access::ComputeSampled},
       {maps.normalmap, vir::Access::ComputeRW},
       {maps.splatmap, vir::Access::ComputeRW},
       {maps.splat_edit, vir::Access::ComputeRW},
       {maps.region, vir::Access::ComputeRead}}
    )
    .with_name("terrain derive")
    .bind_compute_pipeline(render_context.pipeline("terrain_derive"))
    .bind_image(0, 0, maps.heightmap)
    .bind_image(0, 1, maps.ridgemap)
    .bind_image(0, 2, maps.normalmap)
    .bind_image(0, 3, maps.splatmap)
    .bind_image(0, 4, maps.splat_edit)
    .bind_buffer(0, 5, maps.region);
  push(module);
  module.dispatch(groups_x, groups_y, 1);
  std::tie(maps.heightmap, maps.ridgemap, maps.normalmap, maps.splatmap, maps.splat_edit, maps.region) = std::tuple_cat(
    module.end_compute<6>()
  );
}

template <typename PushF>
static auto record_minmax(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  PushF&& push,
  vir::Count groups_x,
  vir::Count groups_y
) -> void {
  ZoneScoped;

  module
    .begin_compute(
      {{maps.heightmap, vir::Access::ComputeSampled},
       {maps.patch_minmax, vir::Access::ComputeRW},
       {maps.region, vir::Access::ComputeRead}}
    )
    .with_name("terrain minmax")
    .bind_compute_pipeline(render_context.pipeline("terrain_minmax"))
    .bind_image(0, 0, maps.heightmap)
    .bind_image(0, 1, maps.patch_minmax)
    .bind_buffer(0, 2, maps.region);
  push(module);
  module.dispatch(groups_x, groups_y, 1);
  std::tie(maps.heightmap, maps.patch_minmax, maps.region) = std::tuple_cat(module.end_compute<3>());
}

auto record_terrain_generate(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  const GPU::TerrainGenerate& settings,
  glm::uvec2 dispatch_texels
) -> void {
  record_generate(
    module,
    render_context,
    maps,
    [&](vir::Module& m) { m.push_constants(settings); },
    (dispatch_texels.x + 15) / 16,
    (dispatch_texels.y + 15) / 16
  );
}

auto record_terrain_generate(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  vir::ValueID settings,
  vir::ValueID groups_x,
  vir::ValueID groups_y
) -> void {
  record_generate(
    module,
    render_context,
    maps,
    [&](vir::Module& m) { m.push_constants_from(settings); },
    groups_x,
    groups_y
  );
}

auto record_terrain_derive(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  const GPU::TerrainDerive& settings,
  glm::uvec2 dispatch_texels
) -> void {
  record_derive(
    module,
    render_context,
    maps,
    [&](vir::Module& m) { m.push_constants(settings); },
    (dispatch_texels.x + 15) / 16,
    (dispatch_texels.y + 15) / 16
  );
}

auto record_terrain_derive(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  vir::ValueID settings,
  vir::ValueID groups_x,
  vir::ValueID groups_y
) -> void {
  record_derive(
    module,
    render_context,
    maps,
    [&](vir::Module& m) { m.push_constants_from(settings); },
    groups_x,
    groups_y
  );
}

auto record_terrain_minmax(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  const GPU::TerrainMinMax& settings,
  glm::uvec2 dispatch_patches
) -> void {
  record_minmax(
    module,
    render_context,
    maps,
    [&](vir::Module& m) { m.push_constants(settings); },
    (dispatch_patches.x + 7) / 8,
    (dispatch_patches.y + 7) / 8
  );
}

auto record_terrain_minmax(
  vir::Module& module,
  const RenderContext& render_context,
  TerrainMapValues& maps,
  vir::ValueID settings,
  vir::ValueID groups_x,
  vir::ValueID groups_y
) -> void {
  record_minmax(
    module,
    render_context,
    maps,
    [&](vir::Module& m) { m.push_constants_from(settings); },
    groups_x,
    groups_y
  );
}

auto Terrain::create(this Terrain& self) -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.resolution.x == 0 || self.resolution.y == 0) {
    return std::unexpected("terrain resolution must be non-zero");
  }
  if (self.patch_count.x == 0 || self.patch_count.y == 0) {
    return std::unexpected("terrain patch count must be non-zero");
  }

  // Brush edits are keyed to the texel grid, so they survive every parameter change except one that
  // moves the grid under them.
  const auto keep_edits = self.height_edit && self.splat_edit &&
                          self.height_edit.get_format() == vir::Format::R32Sfloat &&
                          self.height_edit.get_extent().width == self.resolution.x &&
                          self.height_edit.get_extent().height == self.resolution.y;
  auto height_edit = keep_edits ? std::move(self.height_edit) : Texture{};
  auto splat_edit = keep_edits ? std::move(self.splat_edit) : Texture{};

  self.destroy();

  const auto map_extent = vir::Extent3D{.width = self.resolution.x, .height = self.resolution.y, .depth = 1};
  self.heightmap = Texture::create({
    .format = vir::Format::R16Unorm,
    .extent = map_extent,
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage | vir::ImageUsageFlags::TransferSrc,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.ridgemap = Texture::create({
    .format = vir::Format::R16Sfloat,
    .extent = map_extent,
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.normalmap = Texture::create({
    .format = vir::Format::RG16Sfloat,
    .extent = map_extent,
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.splatmap = Texture::create({
    .format = vir::Format::RGBA8Unorm,
    .extent = map_extent,
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.patch_minmax = Texture::create({
    .format = vir::Format::RG16Unorm,
    .extent = {.width = self.patch_count.x, .height = self.patch_count.y, .depth = 1},
    .usage = vir::ImageUsageFlags::Sampled | vir::ImageUsageFlags::Storage,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });

  if (keep_edits) {
    self.height_edit = std::move(height_edit);
    self.splat_edit = std::move(splat_edit);
  } else {
    create_terrain_edit_maps(self, map_extent);
    self.edits_uninitialized = true;
  }

  if (
    !self.heightmap || !self.ridgemap || !self.normalmap || !self.splatmap || !self.patch_minmax || !self.height_edit ||
    !self.splat_edit
  ) {
    self.destroy();
    return std::unexpected("failed to allocate terrain maps");
  }

  self.heightmap.set_name("terrain heightmap");
  self.ridgemap.set_name("terrain ridgemap");
  self.normalmap.set_name("terrain normalmap");
  self.splatmap.set_name("terrain splatmap");
  self.patch_minmax.set_name("terrain patch minmax");

  return {};
}

auto Terrain::destroy(this Terrain& self) -> void {
  ZoneScoped;

  self.heightmap.destroy();
  self.ridgemap.destroy();
  self.normalmap.destroy();
  self.splatmap.destroy();
  self.patch_minmax.destroy();
  self.height_edit.destroy();
  self.splat_edit.destroy();
}

auto Terrain::clone_edits_from(this Terrain& self, const Terrain& src, RenderContext& render_context) -> void {
  ZoneScoped;

  if (src.edits_uninitialized || !src.height_edit || !src.splat_edit) {
    return;
  }

  const auto extent = src.height_edit.get_extent();
  const auto is_same_extent = self.height_edit && self.height_edit.get_extent().width == extent.width &&
                              self.height_edit.get_extent().height == extent.height;
  if (!self.height_edit || !self.splat_edit || !is_same_extent) {
    create_terrain_edit_maps(self, extent);
  }

  if (!self.height_edit || !self.splat_edit) {
    OX_LOG_ERROR("Failed to allocate terrain edit maps to clone into.");
    return;
  }

  auto module = vir::Module{};
  const auto copy_edit_map = [&module](const Texture& src_map, const Texture& dst_map) -> void {
    const auto source = src_map.import(module, vir::Access::ComputeRW);
    const auto copied = module.copy_image(source, dst_map.import_discarded(module, vir::Access::ComputeRW));
    // exported after the copy, which reaches the source first
    module.export_to(copied, vir::Access::ComputeRW);
    module.export_to(source, vir::Access::ComputeRW);
  };
  copy_edit_map(src.height_edit, self.height_edit);
  copy_edit_map(src.splat_edit, self.splat_edit);
  if (const auto executed = render_context.execute_blocking(std::move(module)); !executed) {
    OX_LOG_ERROR("Failed to copy terrain edit maps ({}).", std::to_underlying(executed.error()));
    return;
  }

  self.edits_uninitialized = false;
}

auto download_terrain_map(const Texture& map, vir::Access rests_at, RenderContext& render_context) -> std::vector<u8> {
  ZoneScoped;

  const auto extent = map.get_extent();
  const auto texel_size = vir::format_texel_block_size(map.get_format());
  const auto byte_size = static_cast<u64>(extent.width) * extent.height * extent.depth * texel_size;

  auto readback = render_context.persistent->allocate_buffer({
    .size = byte_size,
    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    .location = vir::MemoryLocation::GpuToCpu,
    .alignment = texel_size,
    .name = "terrain map readback",
  });
  if (!readback) {
    OX_LOG_ERROR("Failed to allocate a terrain map readback buffer ({}).", std::to_underlying(readback.error()));
    return {};
  }

  OX_DEFER(&) { render_context.persistent->deallocate_buffer(*readback); };

  auto module = vir::Module{};
  const auto source = map.import(module, rests_at);
  const auto downloaded = module.copy_image_to_buffer(source, module.import_buffer(*readback, vir::Access::None));
  module.export_to(downloaded, vir::Access::HostRead);
  module.export_to(source, rests_at);
  if (const auto executed = render_context.execute_blocking(std::move(module)); !executed) {
    OX_LOG_ERROR("Failed to read back a terrain map ({}).", std::to_underlying(executed.error()));
    return {};
  }

  if (!readback->is_mapped() || !render_context.persistent->invalidate_buffer(*readback)) {
    OX_LOG_ERROR("Terrain map readback buffer came back unmapped.");
    return {};
  }

  const auto bytes = readback->mapped_span().first(byte_size);
  return std::vector<u8>(bytes.begin(), bytes.end());
}

auto Terrain::download_edits(this const Terrain& self, RenderContext& render_context) -> TerrainEdits {
  ZoneScoped;

  if (self.edits_uninitialized || !self.height_edit || !self.splat_edit) {
    return {};
  }

  const auto extent = self.height_edit.get_extent();

  return TerrainEdits{
    .resolution = {extent.width, extent.height},
    .height = download_terrain_map(self.height_edit, vir::Access::ComputeRW, render_context),
    .splat = download_terrain_map(self.splat_edit, vir::Access::ComputeRW, render_context),
  };
}

auto Terrain::upload_edits(this Terrain& self, const TerrainEdits& edits) -> void {
  ZoneScoped;

  if (edits.is_empty() || !self.height_edit || !self.splat_edit) {
    return;
  }

  const auto extent = self.height_edit.get_extent();
  if (edits.resolution.x != extent.width || edits.resolution.y != extent.height) {
    OX_LOG_WARN(
      "Terrain edits were authored at {}x{} but the terrain is {}x{}; discarding them.",
      edits.resolution.x,
      edits.resolution.y,
      extent.width,
      extent.height
    );
    return;
  }

  self.height_edit.upload(edits.height, vir::Access::ComputeRW);
  self.splat_edit.upload(edits.splat, vir::Access::ComputeRW);
  self.edits_uninitialized = false;
}

auto Terrain::bake(this Terrain& self, RenderContext& render_context) -> void {
  ZoneScoped;

  if (!self.is_baked()) {
    OX_LOG_ERROR("Terrain::bake called before Terrain::create.");
    return;
  }

  auto generate_settings = self.generate_settings;
  generate_settings.resolution = self.resolution;

  auto derive_settings = self.derive_settings;
  derive_settings.resolution = self.resolution;
  derive_settings.texel_world_size = self.texel_world_size();
  derive_settings.height_range = self.height_range.y - self.height_range.x;

  const auto minmax_settings = GPU::TerrainMinMax{
    .resolution = self.resolution,
    .patch_count = self.patch_count,
  };

  const auto region = GPU::TerrainRegion{};
  const auto edit_clear = vir::ClearValue::rgba_f32(0.0f, 0.0f, 0.0f, 1.0f);

  auto module = vir::Module{};
  auto maps = TerrainMapValues{
    .heightmap = self.heightmap.import_discarded(module, vir::Access::ComputeSampled),
    .ridgemap = self.ridgemap.import_discarded(module, vir::Access::FragmentSampled),
    .normalmap = self.normalmap.import_discarded(module, vir::Access::FragmentSampled),
    .splatmap = self.splatmap.import_discarded(module, vir::Access::FragmentSampled),
    .patch_minmax = self.patch_minmax.import_discarded(module, vir::Access::ComputeSampled),
    .height_edit = self.edits_uninitialized
                     ? module.clear(self.height_edit.import_discarded(module, vir::Access::ComputeRW), edit_clear)
                     : self.height_edit.import(module, vir::Access::ComputeRW),
    .splat_edit = self.edits_uninitialized
                    ? module.clear(self.splat_edit.import_discarded(module, vir::Access::ComputeRW), edit_clear)
                    : self.splat_edit.import(module, vir::Access::ComputeRW),
    .region = module.update_buffer(
      module.transient_buffer({
        .size = sizeof(GPU::TerrainRegion),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .name = "terrain region",
      }),
      std::span{reinterpret_cast<const u8*>(&region), sizeof(region)}
    ),
  };

  record_terrain_generate(module, render_context, maps, generate_settings, self.resolution);
  record_terrain_derive(module, render_context, maps, derive_settings, self.resolution);
  record_terrain_minmax(module, render_context, maps, minmax_settings, self.patch_count);

  module.export_to(maps.heightmap, vir::Access::ComputeSampled);
  module.export_to(maps.ridgemap, vir::Access::FragmentSampled);
  module.export_to(maps.normalmap, vir::Access::FragmentSampled);
  module.export_to(maps.splatmap, vir::Access::FragmentSampled);
  module.export_to(maps.patch_minmax, vir::Access::ComputeSampled);
  module.export_to(maps.height_edit, vir::Access::ComputeRW);
  module.export_to(maps.splat_edit, vir::Access::ComputeRW);
  if (const auto executed = render_context.execute_blocking(std::move(module)); !executed) {
    OX_LOG_ERROR("Failed to bake terrain ({}).", std::to_underlying(executed.error()));
    return;
  }

  self.edits_uninitialized = false;
}

auto Terrain::download_collision_heights(this Terrain& self, RenderContext& render_context) -> void {
  ZoneScoped;

  self.collision_heights.clear();
  self.collision_sample_count = 0;

  if (!self.is_baked()) {
    return;
  }

  const auto extent = self.heightmap.get_extent();
  const auto downloaded = download_terrain_map(self.heightmap, vir::Access::ComputeSampled, render_context);
  if (downloaded.size() < static_cast<usize>(extent.width) * extent.height * sizeof(u16)) {
    return;
  }

  const auto* texels = reinterpret_cast<const u16*>(downloaded.data());

  const auto sample_count = terrain_collision_sample_count(self.collision_resolution);
  const auto width = static_cast<i32>(extent.width);
  const auto height = static_cast<i32>(extent.height);

  const auto texel = [texels, width, height](i32 x, i32 y) -> f32 {
    const auto cx = std::clamp(x, 0, width - 1);
    const auto cy = std::clamp(y, 0, height - 1);
    return static_cast<f32>(texels[static_cast<usize>(cy) * static_cast<usize>(width) + static_cast<usize>(cx)]) /
           65535.0f;
  };

  self.collision_heights.resize(static_cast<usize>(sample_count) * sample_count);
  self.collision_sample_count = sample_count;

  const auto base = self.base_height();
  const auto scale = self.height_scale();
  const auto map_size = glm::vec2(extent.width, extent.height);
  const auto inv_last_sample = 1.0f / static_cast<f32>(sample_count - 1);

  for (auto y = 0_u32; y < sample_count; y++) {
    for (auto x = 0_u32; x < sample_count; x++) {
      const auto uv = glm::vec2(x, y) * inv_last_sample;
      const auto coord = uv * map_size - 0.5f;
      const auto lo = glm::ivec2(glm::floor(coord));
      const auto frac = coord - glm::vec2(lo);

      const auto top = glm::mix(texel(lo.x, lo.y), texel(lo.x + 1, lo.y), frac.x);
      const auto bottom = glm::mix(texel(lo.x, lo.y + 1), texel(lo.x + 1, lo.y + 1), frac.x);

      self.collision_heights[static_cast<usize>(y) * sample_count + x] = base + glm::mix(top, bottom, frac.y) * scale;
    }
  }
}
} // namespace ox
