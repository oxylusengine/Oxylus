#include "Scene/Terrain.hpp"

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
                          self.height_edit.get_extent().width == self.resolution.x &&
                          self.height_edit.get_extent().height == self.resolution.y;
  auto height_edit = keep_edits ? std::move(self.height_edit) : Texture{};
  auto splat_edit = keep_edits ? std::move(self.splat_edit) : Texture{};

  self.destroy();

  const auto map_extent = vuk::Extent3D{.width = self.resolution.x, .height = self.resolution.y, .depth = 1};
  constexpr auto storage_usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage;
  // Terrain maps are addressed by a bounded world rect; repeating would wrap the far edge onto the
  // near one and produce a seam across the whole tile.
  constexpr auto clamped_sampler = vuk::SamplerCreateInfo{
    .magFilter = vuk::Filter::eLinear,
    .minFilter = vuk::Filter::eLinear,
    .mipmapMode = vuk::SamplerMipmapMode::eLinear,
    .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
    .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
    .addressModeW = vuk::SamplerAddressMode::eClampToEdge,
  };

  // 16 bits of normalized range beats fp16 here: over a 2 km altitude span fp16 quantizes to
  // roughly a metre near the top, unorm16 to about 3 cm everywhere.
  self.heightmap = Texture::create(
    {.format = vuk::Format::eR16Unorm, .extent = map_extent, .usage = storage_usage, .sampler_info = clamped_sampler}
  );
  self.ridgemap = Texture::create(
    {.format = vuk::Format::eR16Sfloat, .extent = map_extent, .usage = storage_usage, .sampler_info = clamped_sampler}
  );
  self.normalmap = Texture::create(
    {.format = vuk::Format::eR16G16Sfloat,
     .extent = map_extent,
     .usage = storage_usage,
     .sampler_info = clamped_sampler}
  );
  self.splatmap = Texture::create(
    {.format = vuk::Format::eR8G8B8A8Unorm,
     .extent = map_extent,
     .usage = storage_usage,
     .sampler_info = clamped_sampler}
  );
  self.patch_minmax = Texture::create(
    {.format = vuk::Format::eR16G16Unorm,
     .extent = {.width = self.patch_count.x, .height = self.patch_count.y, .depth = 1},
     .usage = storage_usage,
     .sampler_info = clamped_sampler}
  );

  if (keep_edits) {
    self.height_edit = std::move(height_edit);
    self.splat_edit = std::move(splat_edit);
  } else {
    self.height_edit = Texture::create(
      {.format = vuk::Format::eR16Snorm, .extent = map_extent, .usage = storage_usage, .sampler_info = clamped_sampler}
    );
    self.splat_edit = Texture::create(
      {.format = vuk::Format::eR8G8B8A8Unorm,
       .extent = map_extent,
       .usage = storage_usage,
       .sampler_info = clamped_sampler}
    );
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
  self.height_edit.set_name("terrain height edit");
  self.splat_edit.set_name("terrain splat edit");

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

  auto generate_pass = vuk::make_pass(
    "terrain generate",
    [generate_settings](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeRW) heightmap,
      VUK_IA(vuk::eComputeRW) ridgemap,
      VUK_IA(vuk::eComputeSampled) height_edit
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_generate")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, ridgemap)
        .bind_image(0, 2, height_edit)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, generate_settings)
        .dispatch_invocations_per_pixel(heightmap);

      return std::make_tuple(heightmap, ridgemap, height_edit);
    }
  );

  std::tie(maps.heightmap, maps.ridgemap, maps.height_edit) = generate_pass(
    std::move(maps.heightmap),
    std::move(maps.ridgemap),
    std::move(maps.height_edit)
  );

  terrain_derive_pass(maps, derive_settings, self.resolution);
  terrain_minmax_pass(maps, minmax_settings, self.patch_count);

  // The maps are sampled by the domain shader (heightmap), the patch cull compute pass
  // (patch_minmax) and the visbuffer decode fragment shader (the rest).
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
} // namespace ox
