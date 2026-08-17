#include "Scene/Terrain.hpp"

#include <algorithm>
#include <bit>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Render/RenderContext.hpp"
#include "Utils/Log.hpp"

namespace ox {
static_assert(sizeof(GPU::TerrainErosion) == 80, "TerrainErosion layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainGenerate) == 120, "TerrainGenerate layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainDerive) == 40, "TerrainDerive layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainMinMax) == 16, "TerrainMinMax layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainRegion) == 16, "TerrainRegion layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainBrushHit) == 16, "TerrainBrushHit layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainBrushParams) == 96, "TerrainBrushParams layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainGenerate) <= 128, "push constant blocks must fit the guaranteed 128 bytes");
static_assert(sizeof(GPU::TerrainBrushParams) <= 128, "push constant blocks must fit the guaranteed 128 bytes");

constexpr auto TERRAIN_MAP_SAMPLER = vuk::SamplerCreateInfo{
  .magFilter = vuk::Filter::eLinear,
  .minFilter = vuk::Filter::eLinear,
  .mipmapMode = vuk::SamplerMipmapMode::eLinear,
  .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
  .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
  .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
};

auto create_terrain_edit_maps(Terrain& terrain, const vuk::Extent3D& extent) -> void {
  terrain.height_edit = Texture::create(
    {.format = vuk::Format::eR32Sfloat,
     .extent = extent,
     .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage |
              vuk::ImageUsageFlagBits::eTransferSrc,
     .sampler_info = TERRAIN_MAP_SAMPLER}
  );
  terrain.splat_edit = Texture::create(
    {.format = vuk::Format::eR8G8B8A8Unorm,
     .extent = extent,
     .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage |
              vuk::ImageUsageFlagBits::eTransferSrc,
     .sampler_info = TERRAIN_MAP_SAMPLER}
  );

  terrain.height_edit.set_name("terrain height edit");
  terrain.splat_edit.set_name("terrain splat edit");
}

auto terrain_collision_sample_count(u32 requested) -> u32 {
  return std::bit_ceil(std::clamp(requested, TERRAIN_COLLISION_MIN_SAMPLES, TERRAIN_COLLISION_MAX_SAMPLES));
}

auto terrain_generate_pass(TerrainMaps& maps, const GPU::TerrainGenerate& settings, glm::uvec2 dispatch_texels)
  -> void {
  ZoneScoped;

  auto pass = vuk::make_pass(
    "terrain generate",
    [settings, dispatch_texels](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeRW) heightmap,
      VUK_IA(vuk::eComputeRW) ridgemap,
      VUK_IA(vuk::eComputeSampled) height_edit,
      VUK_BA(vuk::eComputeRead) region
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_generate")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, ridgemap)
        .bind_image(0, 2, height_edit)
        .bind_buffer(0, 3, region)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, settings)
        .dispatch((dispatch_texels.x + 15) / 16, (dispatch_texels.y + 15) / 16, 1);

      return std::make_tuple(heightmap, ridgemap, height_edit, region);
    }
  );

  std::tie(maps.heightmap, maps.ridgemap, maps.height_edit, maps.region) = pass(
    std::move(maps.heightmap),
    std::move(maps.ridgemap),
    std::move(maps.height_edit),
    std::move(maps.region)
  );
}

auto terrain_derive_pass(TerrainMaps& maps, const GPU::TerrainDerive& settings, glm::uvec2 dispatch_texels) -> void {
  ZoneScoped;

  auto pass = vuk::make_pass(
    "terrain derive",
    [settings, dispatch_texels](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) heightmap,
      VUK_IA(vuk::eComputeSampled) ridgemap,
      VUK_IA(vuk::eComputeRW) normalmap,
      VUK_IA(vuk::eComputeRW) splatmap,
      VUK_IA(vuk::eComputeRW) splat_edit,
      VUK_BA(vuk::eComputeRead) region
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_derive")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, ridgemap)
        .bind_image(0, 2, normalmap)
        .bind_image(0, 3, splatmap)
        .bind_image(0, 4, splat_edit)
        .bind_buffer(0, 5, region)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, settings)
        .dispatch((dispatch_texels.x + 15) / 16, (dispatch_texels.y + 15) / 16, 1);

      return std::make_tuple(heightmap, ridgemap, normalmap, splatmap, splat_edit, region);
    }
  );

  std::tie(maps.heightmap, maps.ridgemap, maps.normalmap, maps.splatmap, maps.splat_edit, maps.region) = pass(
    std::move(maps.heightmap),
    std::move(maps.ridgemap),
    std::move(maps.normalmap),
    std::move(maps.splatmap),
    std::move(maps.splat_edit),
    std::move(maps.region)
  );
}

auto terrain_minmax_pass(TerrainMaps& maps, const GPU::TerrainMinMax& settings, glm::uvec2 dispatch_patches) -> void {
  ZoneScoped;

  auto pass = vuk::make_pass(
    "terrain minmax",
    [settings, dispatch_patches](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) heightmap,
      VUK_IA(vuk::eComputeRW) patch_minmax,
      VUK_BA(vuk::eComputeRead) region
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_minmax")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, patch_minmax)
        .bind_buffer(0, 2, region)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, settings)
        .dispatch((dispatch_patches.x + 7) / 8, (dispatch_patches.y + 7) / 8, 1);

      return std::make_tuple(heightmap, patch_minmax, region);
    }
  );

  std::tie(maps.heightmap, maps.patch_minmax, maps.region) = pass(
    std::move(maps.heightmap),
    std::move(maps.patch_minmax),
    std::move(maps.region)
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
                          self.height_edit.get_format() == vuk::Format::eR32Sfloat &&
                          self.height_edit.get_extent().width == self.resolution.x &&
                          self.height_edit.get_extent().height == self.resolution.y;
  auto height_edit = keep_edits ? std::move(self.height_edit) : Texture{};
  auto splat_edit = keep_edits ? std::move(self.splat_edit) : Texture{};

  self.destroy();

  const auto map_extent = vuk::Extent3D{.width = self.resolution.x, .height = self.resolution.y, .depth = 1};
  self.heightmap = Texture::create({
    .format = vuk::Format::eR16Unorm,
    .extent = map_extent,
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage |
             vuk::ImageUsageFlagBits::eTransferSrc,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.ridgemap = Texture::create({
    .format = vuk::Format::eR16Sfloat,
    .extent = map_extent,
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.normalmap = Texture::create({
    .format = vuk::Format::eR16G16Sfloat,
    .extent = map_extent,
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.splatmap = Texture::create({
    .format = vuk::Format::eR8G8B8A8Unorm,
    .extent = map_extent,
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
    .sampler_info = TERRAIN_MAP_SAMPLER,
  });
  self.patch_minmax = Texture::create({
    .format = vuk::Format::eR16G16Unorm,
    .extent = {.width = self.patch_count.x, .height = self.patch_count.y, .depth = 1},
    .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
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
  if (!self.height_edit || !self.splat_edit || self.height_edit.get_extent() != extent) {
    create_terrain_edit_maps(self, extent);
  }

  if (!self.height_edit || !self.splat_edit) {
    OX_LOG_ERROR("Failed to allocate terrain edit maps to clone into.");
    return;
  }

  auto copy_pass = vuk::make_pass(
    "terrain edit copy",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eCopyRead) src_map,
      VUK_IA(vuk::eCopyWrite) dst_map
    ) {
      const auto region = vuk::ImageCopy{
        .srcSubresource =
          {.aspectMask = vuk::format_to_aspect(src_map->format),
           .mipLevel = src_map->base_level,
           .baseArrayLayer = src_map->base_layer,
           .layerCount = src_map->layer_count},
        .dstSubresource =
          {.aspectMask = vuk::format_to_aspect(dst_map->format),
           .mipLevel = dst_map->base_level,
           .baseArrayLayer = dst_map->base_layer,
           .layerCount = dst_map->layer_count},
        .imageExtent = dst_map->base_mip_extent(),
      };
      cmd_list.copy_image(src_map, dst_map, region);

      return std::make_tuple(src_map, dst_map);
    }
  );

  auto [src_height, dst_height] = copy_pass(
    src.height_edit.acquire("terrain height edit", vuk::eComputeRW),
    self.height_edit.discard("terrain height edit")
  );
  auto [src_splat, dst_splat] = copy_pass(
    src.splat_edit.acquire("terrain splat edit", vuk::eComputeRW),
    self.splat_edit.discard("terrain splat edit")
  );

  auto waits = std::array{
    vuk::UntypedValue(std::move(src_height).as_released(vuk::eComputeRW, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(std::move(dst_height).as_released(vuk::eComputeRW, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(std::move(src_splat).as_released(vuk::eComputeRW, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(std::move(dst_splat).as_released(vuk::eComputeRW, vuk::DomainFlagBits::eGraphicsQueue)),
  };
  render_context.wait_on_multiple(waits);

  self.edits_uninitialized = false;
}

auto download_terrain_map(const Texture& map, vuk::Access last_access, RenderContext& render_context)
  -> std::vector<u8> {
  const auto extent = map.get_extent();
  const auto format = map.get_format();
  const auto byte_size = vuk::compute_image_size(format, extent);

  auto staging = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUtoCPU,
    byte_size,
    vuk::format_to_texel_block_size(format)
  );

  auto download_pass = vuk::make_pass(
    "terrain map readback",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eCopyRead) src,
      VUK_BA(vuk::eCopyWrite) dst
    ) {
      const auto region = vuk::BufferImageCopy{
        .bufferOffset = dst->offset,
        .imageSubresource =
          {.aspectMask = vuk::format_to_aspect(src->format),
           .mipLevel = src->base_level,
           .baseArrayLayer = src->base_layer,
           .layerCount = src->layer_count},
        .imageExtent = src->base_mip_extent(),
      };
      cmd_list.copy_image_to_buffer(src, dst, region);

      return std::make_tuple(src, dst);
    }
  );

  auto [src, downloaded] = download_pass(
    map.acquire("terrain map readback source", last_access),
    vuk::acquire_buf("terrain map readback", *staging, vuk::eNone)
  );

  auto waits = std::array{
    vuk::UntypedValue(std::move(src).as_released(last_access, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(std::move(downloaded).as_released(vuk::eHostRead, vuk::DomainFlagBits::eGraphicsQueue)),
  };
  render_context.wait_on_multiple(waits);

  const auto* bytes = reinterpret_cast<const u8*>(staging->mapped_ptr);
  if (bytes == nullptr) {
    OX_LOG_ERROR("Terrain map readback buffer came back unmapped.");
    return {};
  }

  return std::vector<u8>(bytes, bytes + byte_size);
}

auto Terrain::download_edits(this const Terrain& self, RenderContext& render_context) -> TerrainEdits {
  ZoneScoped;

  if (self.edits_uninitialized || !self.height_edit || !self.splat_edit) {
    return {};
  }

  const auto extent = self.height_edit.get_extent();

  return TerrainEdits{
    .resolution = {extent.width, extent.height},
    .height = download_terrain_map(self.height_edit, vuk::eComputeRW, render_context),
    .splat = download_terrain_map(self.splat_edit, vuk::eComputeRW, render_context),
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

  self.height_edit.upload(edits.height, vuk::eComputeRW);
  self.splat_edit.upload(edits.splat, vuk::eComputeRW);
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

  auto maps = TerrainMaps{
    .heightmap = self.heightmap.discard("terrain heightmap"),
    .ridgemap = self.ridgemap.discard("terrain ridgemap"),
    .normalmap = self.normalmap.discard("terrain normalmap"),
    .splatmap = self.splatmap.discard("terrain splatmap"),
    .patch_minmax = self.patch_minmax.discard("terrain patch minmax"),
    .height_edit = self.edits_uninitialized
                     ? vuk::clear_image(self.height_edit.discard("terrain height edit"), vuk::Black<f32>)
                     : self.height_edit.acquire("terrain height edit", vuk::eComputeRW),
    .splat_edit = self.edits_uninitialized
                    ? vuk::clear_image(self.splat_edit.discard("terrain splat edit"), vuk::Black<f32>)
                    : self.splat_edit.acquire("terrain splat edit", vuk::eComputeRW),
    .region = render_context.scratch_buffer(GPU::TerrainRegion{}),
  };
  self.edits_uninitialized = false;

  terrain_generate_pass(maps, generate_settings, self.resolution);
  terrain_derive_pass(maps, derive_settings, self.resolution);
  terrain_minmax_pass(maps, minmax_settings, self.patch_count);

  auto waits = std::array{
    vuk::UntypedValue(std::move(maps.heightmap).as_released(vuk::eComputeSampled, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(std::move(maps.ridgemap).as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(
      std::move(maps.normalmap).as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue)
    ),
    vuk::UntypedValue(std::move(maps.splatmap).as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(
      std::move(maps.patch_minmax).as_released(vuk::eComputeSampled, vuk::DomainFlagBits::eGraphicsQueue)
    ),
    vuk::UntypedValue(std::move(maps.height_edit).as_released(vuk::eComputeRW, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(std::move(maps.splat_edit).as_released(vuk::eComputeRW, vuk::DomainFlagBits::eGraphicsQueue)),
  };
  render_context.wait_on_multiple(waits);
}

auto Terrain::download_collision_heights(this Terrain& self, RenderContext& render_context) -> void {
  ZoneScoped;

  self.collision_heights.clear();
  self.collision_sample_count = 0;

  if (!self.is_baked()) {
    return;
  }

  const auto extent = self.heightmap.get_extent();
  const auto format = self.heightmap.get_format();
  auto staging = render_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUtoCPU,
    vuk::compute_image_size(format, extent),
    vuk::format_to_texel_block_size(format)
  );

  auto download_pass = vuk::make_pass(
    "terrain heightmap readback",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eCopyRead) heightmap,
      VUK_BA(vuk::eCopyWrite) dst
    ) {
      const auto region = vuk::BufferImageCopy{
        .bufferOffset = dst->offset,
        .imageSubresource =
          {.aspectMask = vuk::format_to_aspect(heightmap->format),
           .mipLevel = heightmap->base_level,
           .baseArrayLayer = heightmap->base_layer,
           .layerCount = heightmap->layer_count},
        .imageExtent = heightmap->base_mip_extent(),
      };
      cmd_list.copy_image_to_buffer(heightmap, dst, region);

      return std::make_tuple(heightmap, dst);
    }
  );

  auto [heightmap, downloaded] = download_pass(
    self.heightmap.acquire("terrain heightmap", vuk::eComputeSampled),
    vuk::acquire_buf("terrain heightmap readback", *staging, vuk::eNone)
  );

  auto waits = std::array{
    vuk::UntypedValue(std::move(heightmap).as_released(vuk::eComputeSampled, vuk::DomainFlagBits::eGraphicsQueue)),
    vuk::UntypedValue(std::move(downloaded).as_released(vuk::eHostRead, vuk::DomainFlagBits::eGraphicsQueue)),
  };
  render_context.wait_on_multiple(waits);

  const auto* texels = reinterpret_cast<const u16*>(staging->mapped_ptr);
  if (texels == nullptr) {
    OX_LOG_ERROR("Terrain heightmap readback buffer came back unmapped.");
    return;
  }

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
