#include "Scene/Terrain.hpp"

#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Render/RenderContext.hpp"
#include "Utils/Log.hpp"

namespace ox {
// scene.slang mirrors these by hand, and the shaders are compiled with GLSLForceScalarLayout, so a
// silent size change here would corrupt push constants rather than fail to compile.
static_assert(sizeof(GPU::TerrainErosion) == 80, "TerrainErosion layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainGenerate) == 120, "TerrainGenerate layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainDerive) == 40, "TerrainDerive layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainMinMax) == 16, "TerrainMinMax layout drifted from scene.slang");
static_assert(sizeof(GPU::TerrainGenerate) <= 128, "push constant blocks must fit the guaranteed 128 bytes");

auto Terrain::create(this Terrain& self) -> std::expected<void, std::string> {
  ZoneScoped;

  if (self.resolution.x == 0 || self.resolution.y == 0) {
    return std::unexpected("terrain resolution must be non-zero");
  }
  if (self.patch_count.x == 0 || self.patch_count.y == 0) {
    return std::unexpected("terrain patch count must be non-zero");
  }

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

  if (!self.heightmap || !self.ridgemap || !self.normalmap || !self.splatmap || !self.patch_minmax) {
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

  auto heightmap_attachment = self.heightmap.discard("terrain heightmap");
  auto ridgemap_attachment = self.ridgemap.discard("terrain ridgemap");
  auto normalmap_attachment = self.normalmap.discard("terrain normalmap");
  auto splatmap_attachment = self.splatmap.discard("terrain splatmap");
  auto patch_minmax_attachment = self.patch_minmax.discard("terrain patch minmax");

  auto generate_pass = vuk::make_pass(
    "terrain generate",
    [generate_settings](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeRW) heightmap,
      VUK_IA(vuk::eComputeRW) ridgemap
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_generate")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, ridgemap)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, generate_settings)
        .dispatch_invocations_per_pixel(heightmap);

      return std::make_tuple(heightmap, ridgemap);
    }
  );

  std::tie(heightmap_attachment, ridgemap_attachment) = generate_pass(
    std::move(heightmap_attachment),
    std::move(ridgemap_attachment)
  );

  auto derive_pass = vuk::make_pass(
    "terrain derive",
    [derive_settings](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) heightmap,
      VUK_IA(vuk::eComputeSampled) ridgemap,
      VUK_IA(vuk::eComputeRW) normalmap,
      VUK_IA(vuk::eComputeRW) splatmap
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_derive")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, ridgemap)
        .bind_image(0, 2, normalmap)
        .bind_image(0, 3, splatmap)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, derive_settings)
        .dispatch_invocations_per_pixel(normalmap);

      return std::make_tuple(heightmap, ridgemap, normalmap, splatmap);
    }
  );

  std::tie(heightmap_attachment, ridgemap_attachment, normalmap_attachment, splatmap_attachment) = derive_pass(
    std::move(heightmap_attachment),
    std::move(ridgemap_attachment),
    std::move(normalmap_attachment),
    std::move(splatmap_attachment)
  );

  auto minmax_pass = vuk::make_pass(
    "terrain minmax",
    [minmax_settings](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeSampled) heightmap,
      VUK_IA(vuk::eComputeRW) patch_minmax
    ) {
      cmd_list //
        .bind_compute_pipeline("terrain_minmax")
        .bind_image(0, 0, heightmap)
        .bind_image(0, 1, patch_minmax)
        .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, minmax_settings)
        .dispatch_invocations_per_pixel(patch_minmax);

      return std::make_tuple(heightmap, patch_minmax);
    }
  );

  std::tie(heightmap_attachment, patch_minmax_attachment) = minmax_pass(
    std::move(heightmap_attachment),
    std::move(patch_minmax_attachment)
  );

  // The maps are sampled by the domain shader (heightmap), the patch cull compute pass
  // (patch_minmax) and the visbuffer decode fragment shader (the rest).
  auto waits = std::array{
    vuk::UntypedValue(
      std::move(heightmap_attachment).as_released(vuk::eComputeSampled, vuk::DomainFlagBits::eGraphicsQueue)
    ),
    vuk::UntypedValue(
      std::move(ridgemap_attachment).as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue)
    ),
    vuk::UntypedValue(
      std::move(normalmap_attachment).as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue)
    ),
    vuk::UntypedValue(
      std::move(splatmap_attachment).as_released(vuk::eFragmentSampled, vuk::DomainFlagBits::eGraphicsQueue)
    ),
    vuk::UntypedValue(
      std::move(patch_minmax_attachment).as_released(vuk::eComputeSampled, vuk::DomainFlagBits::eGraphicsQueue)
    ),
  };
  render_context.wait_on_multiple(waits);
}
} // namespace ox
