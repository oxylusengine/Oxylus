#include "Render/Renderer.hpp"

#include <vir/Module.hpp>
#include <vir/Types.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Core/ModuleRegistry.hpp"
#include "Core/VFS.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/Samplers.hpp"

namespace ox {
static_assert(ModuleHasUpdate<Renderer>, "Renderer::update must be registered as a module update");

static auto is_two_component_format(vir::Format format) -> bool {
  switch (format) {
    case vir::Format::BC5UnormBlock    :
    case vir::Format::BC5SnormBlock    :
    case vir::Format::EACRG11UnormBlock:
    case vir::Format::EACRG11SnormBlock:
    case vir::Format::RG8Unorm         :
    case vir::Format::RG8Snorm         :
    case vir::Format::RG16Unorm        :
    case vir::Format::RG16Snorm        :
    case vir::Format::RG16Sfloat       : return true;
    default                            : return false;
  }
}

auto to_gpu_material(AssetManager& asset_man, RenderContext& render_context, const Material& material)
  -> GPU::Material {
  ZoneScoped;

  const auto uuid_to_image_index = [&asset_man](const UUID& uuid) -> option<u32> {
    if (!uuid || !asset_man.is_loaded(uuid)) {
      return nullopt;
    }

    auto texture = asset_man.get_texture(uuid);
    return texture->get_view_index();
  };

  const auto albedo_image_index = uuid_to_image_index(material.albedo_texture);
  const auto normal_image_index = uuid_to_image_index(material.normal_texture);
  const auto emissive_image_index = uuid_to_image_index(material.emissive_texture);
  const auto metallic_roughness_image_index = uuid_to_image_index(material.metallic_roughness_texture);
  const auto occlusion_image_index = uuid_to_image_index(material.occlusion_texture);
  auto sampler_index = 0_u32;

  auto flags = GPU::MaterialFlag::None;
  if (albedo_image_index.has_value()) {
    flags |= GPU::MaterialFlag::HasAlbedoImage;

    auto texture = asset_man.get_texture(material.albedo_texture);
    sampler_index = texture->get_sampler_index();

    auto sampler_info = vir::SamplerInfo{};
    switch (material.sampling_mode) {
      case SamplingMode::LinearRepeated          : sampler_info = LinearSamplerRepeated; break;
      case SamplingMode::LinearClamped           : sampler_info = LinearSamplerClamped; break;
      case SamplingMode::NearestRepeated         : sampler_info = NearestSamplerRepeated; break;
      case SamplingMode::NearestClamped          : sampler_info = NearestSamplerClamped; break;
      case SamplingMode::LinearRepeatedAnisotropy: sampler_info = LinearSamplerRepeatedAnisotropy; break;
    }
    const auto material_sampler = render_context.cached_sampler(sampler_info);
    sampler_index = SlotMap_decode_id(material_sampler).index;
  }

  if (normal_image_index.has_value()) {
    flags |= GPU::MaterialFlag::HasNormalImage;

    auto texture = asset_man.get_texture(material.normal_texture);
    if (texture && is_two_component_format(texture->get_format())) {
      flags |= GPU::MaterialFlag::NormalTwoComponent;
    }
  }
  flags |= material.flip_normal_y ? GPU::MaterialFlag::NormalFlipY : GPU::MaterialFlag::None;
  flags |= emissive_image_index.has_value() ? GPU::MaterialFlag::HasEmissiveImage : GPU::MaterialFlag::None;
  flags |= metallic_roughness_image_index.has_value() ? GPU::MaterialFlag::HasMetallicRoughnessImage
                                                      : GPU::MaterialFlag::None;
  flags |= occlusion_image_index.has_value() ? GPU::MaterialFlag::HasOcclusionImage : GPU::MaterialFlag::None;

  return GPU::Material{
    .albedo_color =
      glm::u16vec4{
        glm::packHalf1x16(material.albedo_color.x),
        glm::packHalf1x16(material.albedo_color.y),
        glm::packHalf1x16(material.albedo_color.z),
        glm::packHalf1x16(material.albedo_color.w),
      },
    .emissive_color =
      glm::u16vec3{
        glm::packHalf1x16(material.emissive_color.x),
        glm::packHalf1x16(material.emissive_color.y),
        glm::packHalf1x16(material.emissive_color.z),
      },
    .roughness_factor = glm::packHalf1x16(material.roughness_factor),
    .metallic_factor = glm::packHalf1x16(material.metallic_factor),
    .alpha_cutoff = glm::packHalf1x16(material.alpha_cutoff),
    .normal_scale = glm::packHalf1x16(material.normal_scale),
    .occlusion_strength = glm::packHalf1x16(material.occlusion_strength),
    .flags = flags,
    .sampler_index = sampler_index,
    .albedo_image_index = albedo_image_index.value_or(0_u32),
    .normal_image_index = normal_image_index.value_or(0_u32),
    .emissive_image_index = emissive_image_index.value_or(0_u32),
    .metallic_roughness_image_index = metallic_roughness_image_index.value_or(0_u32),
    .occlusion_image_index = occlusion_image_index.value_or(0_u32),
    .uv_size =
      glm::u16vec2{
        glm::packHalf1x16(material.uv_size.x),
        glm::packHalf1x16(material.uv_size.y),
      },
    .uv_offset = glm::u16vec2{
      glm::packHalf1x16(material.uv_offset.x),
      glm::packHalf1x16(material.uv_offset.y),
    },
  };
}

// stages the dirty materials, or all of them once the buffer had to grow, and copies them in with a
// program of their own. It rides the current frame's allocator, so it lands ahead of the frame
static auto upload_dirty_materials(
  RenderContext& render_context,
  BufferID& buffer,
  std::span<const GPU::Material> gpu_data,
  std::span<const usize> dirty_indices
) -> bool {
  ZoneScoped;
  memory::ScopedStack stack;

  constexpr auto element_size = sizeof(GPU::Material);
  const auto resized = render_context.resize_buffer(buffer, gpu_data.size_bytes(), "materials");
  if (!resized) {
    return false;
  }

  const auto is_full = *resized || dirty_indices.size() * 5 >= gpu_data.size() * 2;
  const auto staged_count = is_full ? gpu_data.size() : dirty_indices.size();
  auto staged = stack.alloc<GPU::Material>(staged_count);
  auto regions = std::vector<vir::BufferCopy>{};
  if (is_full) {
    std::ranges::copy(gpu_data, staged.begin());
    regions.push_back({.src_offset = 0, .dst_offset = 0, .size = gpu_data.size_bytes()});
  } else {
    for (usize i = 0; i < staged_count; i++) {
      staged[i] = gpu_data[dirty_indices[i]];
    }

    for (usize i = 0; i < staged_count;) {
      const auto start_index = dirty_indices[i];
      auto run_length = 1_sz;
      while (i + run_length < staged_count && dirty_indices[i + run_length] == start_index + run_length) {
        ++run_length;
      }

      regions.push_back({
        .src_offset = i * element_size,
        .dst_offset = start_index * element_size,
        .size = run_length * element_size,
      });
      i += run_length;
    }
  }

  const auto staging = render_context.upload_frame_buffer(
    std::span(reinterpret_cast<const u8*>(staged.data()), staged.size_bytes()),
    "material staging"
  );
  if (!staging) {
    return false;
  }

  auto module = vir::Module{};
  const auto src = module.import_buffer(*staging, vir::Access::HostWrite);
  auto dst = module.import_buffer(render_context.buffer(buffer), vir::Access::MemoryRead);
  for (const auto& region : regions) {
    dst = module.copy_buffer(src, dst, region);
  }

  module.export_to(dst, vir::Access::MemoryRead);
  auto program = render_context.compile(std::move(module));
  if (!program) {
    return false;
  }

  return render_context.execute(*program).has_value();
}

auto Renderer::new_instance(Scene& scene) -> std::unique_ptr<RendererInstance> {
  ZoneScoped;

  if (!initalized) {
    OX_LOG_ERROR("Renderer must be initialized before creating instances!");
    return nullptr;
  }

  auto instance = std::make_unique<RendererInstance>(scene, *this);
  return instance;
}

auto Renderer::init(this Renderer& self) -> std::expected<void, std::string> {
  if (self.initalized)
    return std::unexpected("Renderer already initialized!");

  self.initalized = true;

  self.render_context = &App::get_rendercontext();

  self.render_context->wait();

  // --- Shaders ---
  auto& vfs = App::get_vfs();
  auto shaders_dir = vfs.resolve_physical_dir(VFS::APP_DIR, "Shaders");
  auto shader_file = AssetFile::unpack(shaders_dir / "engine.oxpack");
  if (!shader_file.has_value()) {
    return std::unexpected("Cannot initialize renderer shaders!");
  }

  for (const auto& entry : shader_file->entries) {
    const auto* pipeline_data = std::get_if<ShaderPipelineData>(&entry.data);
    if (!pipeline_data) {
      continue;
    }

    self.render_context->create_pipeline(*pipeline_data);
  }

  struct Vertex {
    alignas(4) glm::vec3 position = {};
    alignas(4) glm::vec2 uv = {};
  };
  std::vector<Vertex> vertices(4);
  vertices[0].position = glm::vec3{-1.f, -1.f, 0.f};
  vertices[0].uv = glm::vec2{0.f, 0.f};

  vertices[1].position = glm::vec3{1.f, -1.f, 0.f};
  vertices[1].uv = glm::vec2{1.f, 0.f};

  vertices[2].position = glm::vec3{1.f, 1.f, 0.f};
  vertices[2].uv = glm::vec2{1.f, 1.f};

  vertices[3].position = glm::vec3{-1.f, 1.f, 0.f};
  vertices[3].uv = glm::vec2{0.f, 1.f};

  auto indices = std::vector<uint32_t>{0, 1, 2, 2, 3, 0};

  const auto upload = [&](BufferID& id, std::span<const u8> data, std::string_view name) {
    auto buffer = self.render_context->allocate_buffer({
      .size = data.size_bytes(),
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .location = vir::MemoryLocation::GpuOnly,
      .name = std::string(name),
    });
    auto staging = buffer ? self.render_context->allocate_staging(data) : std::unexpected(buffer.error());
    if (!buffer || !staging) {
      return false;
    }

    auto module = vir::Module{};
    const auto copied = module.copy_buffer(
      module.import_buffer(*staging, vir::Access::HostWrite),
      module.import_buffer(self.render_context->buffer(*buffer), vir::Access::None)
    );
    module.export_to(copied, vir::Access::VertexRead | vir::Access::IndexRead);
    id = *buffer;
    return self.render_context->submit_upload(std::move(module), {*staging}, nullptr).has_value();
  };
  if (
    !upload(
      self.quad_vertex_buffer,
      std::span(reinterpret_cast<const u8*>(vertices.data()), ox::size_bytes(vertices)),
      "quad vertices"
    ) ||
    !upload(
      self.quad_index_buffer,
      std::span(reinterpret_cast<const u8*>(indices.data()), ox::size_bytes(indices)),
      "quad indices"
    )
  ) {
    return std::unexpected("Cannot upload the quad buffers!");
  }

  if (!self.render_context->resize_buffer(self.materials_buffer, sizeof(GPU::Material), "materials")) {
    return std::unexpected("Cannot allocate the materials buffer!");
  }

  return {};
}

auto Renderer::update(this Renderer& self, const Timestep&) -> void {
  ZoneScoped;

  self.sync_materials();
}

auto Renderer::sync_materials(this Renderer& self) -> void {
  ZoneScoped;

  if (!self.initalized || !App::has_mod<AssetManager>()) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();
  const auto dirty_material_ids = asset_man.get_dirty_material_ids();

  for (const auto dirty_id : dirty_material_ids) {
    const auto material = asset_man.get_material(dirty_id);
    if (!material) {
      continue;
    }

    const auto index = static_cast<usize>(SlotMap_decode_id(dirty_id).index);
    if (index >= self.gpu_materials.size()) {
      self.gpu_materials.resize(index + 1, {});
    }

    self.gpu_materials[index] = to_gpu_material(asset_man, *self.render_context, *material.value);
    self.pending_material_indices.emplace_back(index);
  }

  if (self.pending_material_indices.empty()) {
    return;
  }

  std::ranges::sort(self.pending_material_indices);
  const auto unique_end = std::ranges::unique(self.pending_material_indices);
  self.pending_material_indices.erase(unique_end.begin(), unique_end.end());

  // outside a frame there is no allocator to stage into, so the indices wait for the next one
  if (
    upload_dirty_materials(
      *self.render_context,
      self.materials_buffer,
      self.gpu_materials,
      self.pending_material_indices
    )
  ) {
    self.pending_material_indices.clear();
  }
}

auto Renderer::deinit(this Renderer& self) -> std::expected<void, std::string> {
  ZoneScoped;

  for (auto* buffer : {&self.quad_vertex_buffer, &self.quad_index_buffer, &self.materials_buffer}) {
    if (*buffer != BufferID::Invalid) {
      self.render_context->destroy_buffer(*buffer);
      *buffer = BufferID::Invalid;
    }
  }

  return {};
}

} // namespace ox
