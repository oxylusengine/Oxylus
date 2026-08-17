#include "Render/Renderer.hpp"

#include <meshoptimizer.h>
#include <vuk/runtime/CommandBuffer.hpp>
#include <vuk/vsl/Core.hpp>

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Core/ModuleRegistry.hpp"
#include "Core/VFS.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RendererInstance.hpp"
#include "Render/Utils/VukCommon.hpp"

namespace ox {
static_assert(ModuleHasUpdate<Renderer>, "Renderer::update must be registered as a module update");

static auto is_two_component_format(vuk::Format format) -> bool {
  switch (format) {
    case vuk::Format::eBc5UnormBlock:
    case vuk::Format::eBc5SnormBlock:
    case vuk::Format::eEacR11G11UnormBlock:
    case vuk::Format::eEacR11G11SnormBlock:
    case vuk::Format::eR8G8Unorm:
    case vuk::Format::eR8G8Snorm:
    case vuk::Format::eR16G16Unorm:
    case vuk::Format::eR16G16Snorm:
    case vuk::Format::eR16G16Sfloat       : return true;
    default                               : return false;
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

    auto texture_sampler = render_context.resources.samplers.copy_slot(texture->get_sampler_id());

    vuk::SamplerCreateInfo sampler_ci = {};
    switch (material.sampling_mode) {
      case SamplingMode::LinearRepeated          : sampler_ci = vuk::LinearSamplerRepeated; break;
      case SamplingMode::LinearClamped           : sampler_ci = vuk::LinearSamplerClamped; break;
      case SamplingMode::NearestRepeated         : sampler_ci = vuk::NearestSamplerRepeated; break;
      case SamplingMode::NearestClamped          : sampler_ci = vuk::NearestSamplerClamped; break;
      case SamplingMode::LinearRepeatedAnisotropy: sampler_ci = vuk::LinearSamplerRepeatedAnisotropy; break;
    }
    auto material_sampler = render_context.runtime->acquire_sampler(sampler_ci, render_context.num_frames);
    if (!texture_sampler || texture_sampler->id != material_sampler.id) {
      auto sampler_id = render_context.allocate_sampler(sampler_ci);
      sampler_index = SlotMap_decode_id(sampler_id).index;
    }
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
        meshopt_quantizeHalf(material.albedo_color.x),
        meshopt_quantizeHalf(material.albedo_color.y),
        meshopt_quantizeHalf(material.albedo_color.z),
        meshopt_quantizeHalf(material.albedo_color.w),
      },
    .emissive_color =
      glm::u16vec3{
        meshopt_quantizeHalf(material.emissive_color.x),
        meshopt_quantizeHalf(material.emissive_color.y),
        meshopt_quantizeHalf(material.emissive_color.z),
      },
    .roughness_factor = meshopt_quantizeHalf(material.roughness_factor),
    .metallic_factor = meshopt_quantizeHalf(material.metallic_factor),
    .alpha_cutoff = meshopt_quantizeHalf(material.alpha_cutoff),
    .flags = flags,
    .sampler_index = sampler_index,
    .albedo_image_index = albedo_image_index.value_or(0_u32),
    .normal_image_index = normal_image_index.value_or(0_u32),
    .emissive_image_index = emissive_image_index.value_or(0_u32),
    .metallic_roughness_image_index = metallic_roughness_image_index.value_or(0_u32),
    .occlusion_image_index = occlusion_image_index.value_or(0_u32),
    .uv_size =
      glm::u16vec2{
        meshopt_quantizeHalf(material.uv_size.x),
        meshopt_quantizeHalf(material.uv_size.y),
      },
    .uv_offset = glm::u16vec2{
      meshopt_quantizeHalf(material.uv_offset.x),
      meshopt_quantizeHalf(material.uv_offset.y),
    },
  };
}

auto upload_dirty_elements(
  RenderContext& render_context,
  vuk::Unique<vuk::Buffer>& buffer,
  std::span<GPU::Material> gpu_data,
  std::span<const usize> dirty_indices
) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  constexpr auto element_size = sizeof(GPU::Material);
  const auto data_size_bytes = gpu_data.size_bytes();

  const auto rebuild_needed = !buffer || buffer->size < data_size_bytes;
  buffer = render_context.resize_buffer(std::move(buffer), vuk::MemoryUsage::eGPUonly, data_size_bytes);

  if (rebuild_needed || dirty_indices.size() * 5 >= gpu_data.size() * 2) {
    return render_context.upload_staging(gpu_data, *buffer);
  }

  const auto dirty_count = dirty_indices.size();
  auto upload_buffer = render_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, dirty_count * element_size);
  auto* dst_ptr = reinterpret_cast<GPU::Material*>(upload_buffer->mapped_ptr);
  for (usize i = 0; i < dirty_count; ++i) {
    dst_ptr[i] = gpu_data[dirty_indices[i]];
  }

  struct CopyRange {
    usize src_offset = 0;
    usize dst_offset = 0;
    usize size_bytes = 0;
  };

  auto ranges = std::vector<CopyRange>();
  ranges.reserve(dirty_count);
  for (usize i = 0; i < dirty_count;) {
    const auto start_index = dirty_indices[i];
    auto run_length = 1_sz;
    while (i + run_length < dirty_count && dirty_indices[i + run_length] == start_index + run_length) {
      ++run_length;
    }
    ranges.push_back({i * element_size, start_index * element_size, run_length * element_size});
    i += run_length;
  }

  auto update_pass = vuk::make_pass(
    "update materials",
    [copy_ranges = std::move(ranges)](
      vuk::CommandBuffer& cmd_list,
      VUK_BA(vuk::Access::eTransferRead) src_buffer,
      VUK_BA(vuk::Access::eTransferWrite) dst_buffer
    ) {
      for (const auto& r : copy_ranges) {
        cmd_list.copy_buffer(
          src_buffer->subrange(r.src_offset, r.size_bytes),
          dst_buffer->subrange(r.dst_offset, r.size_bytes)
        );
      }
      return dst_buffer;
    }
  );

  auto buffer_handle = vuk::acquire_buf("materials", *buffer, vuk::Access::eMemoryRead);
  return update_pass(std::move(upload_buffer), std::move(buffer_handle));
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

  self.quad_vertex_buffer = self.render_context->resize_buffer(
    std::move(self.quad_vertex_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(vertices)
  );
  auto upload_quad_vertex_buffer = self.render_context->upload_staging(std::span(vertices), *self.quad_vertex_buffer);

  self.quad_index_buffer = self.render_context->resize_buffer(
    std::move(self.quad_index_buffer),
    vuk::MemoryUsage::eGPUonly,
    ox::size_bytes(indices)
  );
  auto upload_quad_index_buffer = self.render_context->upload_staging(std::span(indices), *self.quad_index_buffer);

  self.render_context->wait_on(std::move(upload_quad_vertex_buffer));
  self.render_context->wait_on(std::move(upload_quad_index_buffer));

  self.materials_buffer = self.render_context->allocate_buffer_super(vuk::MemoryUsage::eGPUonly, sizeof(GPU::Material));

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

  if (!self.render_context->frame_allocator.has_value()) {
    return;
  }

  std::ranges::sort(self.pending_material_indices);
  const auto unique_end = std::ranges::unique(self.pending_material_indices);
  self.pending_material_indices.erase(unique_end.begin(), unique_end.end());

  self.render_context->submit_now(upload_dirty_elements(
    *self.render_context,
    self.materials_buffer,
    self.gpu_materials,
    self.pending_material_indices
  ));

  self.pending_material_indices.clear();
}

auto Renderer::get_materials_buffer(this Renderer& self) -> vuk::Value<vuk::Buffer> {
  ZoneScoped;

  return vuk::acquire_buf("materials", *self.materials_buffer, vuk::Access::eMemoryRead);
}

auto Renderer::deinit(this Renderer& self) -> std::expected<void, std::string> {
  ZoneScoped;

  return {};
}

} // namespace ox
