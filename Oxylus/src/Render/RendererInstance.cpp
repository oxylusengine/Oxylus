#include "Render/RendererInstance.hpp"

#include "Asset/AssetManager.hpp"
#include "Asset/Model.hpp"
#include "Asset/Texture.hpp"
#include "Core/App.hpp"
#include "Core/Enum.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Render/Utils/VukCommon.hpp"
#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
static constexpr auto sampler_min_clamp_reduction_mode = VkSamplerReductionModeCreateInfo{
  .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
  .pNext = nullptr,
  .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
};
static constexpr auto hiz_sampler_info = vuk::SamplerCreateInfo{
  .pNext = &sampler_min_clamp_reduction_mode,
  .magFilter = vuk::Filter::eLinear,
  .minFilter = vuk::Filter::eLinear,
  .mipmapMode = vuk::SamplerMipmapMode::eNearest,
  .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
  .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
};

template <>
struct RendererInstance::BufferTraits<GPU::Transforms> {
  using offset_type = u64;
  static constexpr std::string_view buffer_name = "transforms";
  static constexpr std::string_view pass_name = "update scene transforms";

  static auto get_buffer_ref(auto& self) -> auto& { return self.transforms_buffer; }
  static auto& get_prepared_buffer_ref(auto& self) { return self.prepared_frame.transforms_buffer; }

  static auto get_index(const auto& dirty_id) -> usize { return SlotMap_decode_id(dirty_id).index; }

  static auto get_element(const auto& gpu_data, usize index) -> const auto& { return gpu_data[index]; }
};

template <>
struct RendererInstance::BufferTraits<GPU::Material> {
  using offset_type = u32;
  static constexpr std::string_view buffer_name = "materials";
  static constexpr std::string_view pass_name = "update scene materials";

  static auto get_buffer_ref(auto& self) -> auto& { return self.materials_buffer; }
  static auto& get_prepared_buffer_ref(auto& self) { return self.prepared_frame.materials_buffer; }

  static auto get_index(const auto& dirty_id) -> usize { return static_cast<usize>(dirty_id); }

  static auto get_element(const auto& gpu_data, usize index) -> const auto& { return gpu_data[index]; }
};

template <typename T>
auto update_gpu_buffer(auto& self, auto& vk_context, const auto& gpu_data, const auto& dirty_ids) -> void {
  using traits = RendererInstance::BufferTraits<T>;

  const auto data_size_bytes = gpu_data.size_bytes();
  constexpr auto element_size = sizeof(T);

  auto& buffer_ref = traits::get_buffer_ref(self);

  const auto rebuild_needed = !buffer_ref || buffer_ref->size <= data_size_bytes;
  buffer_ref = vk_context.resize_buffer(std::move(buffer_ref), vuk::MemoryUsage::eGPUonly, data_size_bytes);

  if (rebuild_needed) {
    traits::get_prepared_buffer_ref(self) = vk_context.upload_staging(gpu_data, *buffer_ref);
  } else {
    const auto dirty_count = dirty_ids.size();
    const auto dirty_size_bytes = dirty_count * element_size;

    auto upload_buffer = vk_context.alloc_transient_buffer(vuk::MemoryUsage::eCPUtoGPU, dirty_size_bytes);
    auto* dst_ptr = reinterpret_cast<T*>(upload_buffer->mapped_ptr);

    std::vector<typename traits::offset_type> upload_offsets;
    upload_offsets.reserve(dirty_count);

    for (const auto& [i, dirty_id] : std::views::zip(std::views::iota(0_sz), dirty_ids)) {
      const auto index = traits::get_index(dirty_id);
      const auto& element = traits::get_element(gpu_data, index);
      std::memcpy(dst_ptr + i, &element, element_size);
      upload_offsets.push_back(static_cast<typename traits::offset_type>(index * element_size));
    }

    auto update_pass = vuk::make_pass(
      traits::pass_name,
      [upload_offsets](
        vuk::CommandBuffer& cmd_list,
        VUK_BA(vuk::Access::eTransferRead) src_buffer,
        VUK_BA(vuk::Access::eTransferWrite) dst_buffer
      ) {
        for (const auto& [i, offset] : std::views::zip(std::views::iota(0_sz), upload_offsets)) {
          const auto src_subrange = src_buffer->subrange(i * element_size, element_size);
          const auto dst_subrange = dst_buffer->subrange(offset, element_size);
          cmd_list.copy_buffer(src_subrange, dst_subrange);
        }
        return dst_buffer;
      }
    );

    auto buffer_handle = vuk::acquire_buf(traits::buffer_name, *buffer_ref, vuk::Access::eMemoryRead);
    traits::get_prepared_buffer_ref(self) = update_pass(std::move(upload_buffer), std::move(buffer_handle));
  }
}

template <typename T>
auto update_buffer_if_dirty(auto& self, auto& vk_context, const auto& gpu_data, const auto& dirty_ids) -> void {
  using traits = RendererInstance::BufferTraits<T>;

  if (!dirty_ids.empty()) {
    update_gpu_buffer<T>(self, vk_context, gpu_data, dirty_ids);
  } else {
    auto& buffer_ref = traits::get_buffer_ref(self);
    if (buffer_ref) {
      traits::get_prepared_buffer_ref(
        self
      ) = vuk::acquire_buf(traits::buffer_name, *buffer_ref, vuk::Access::eMemoryRead);
    }
  }
}

auto get_frustum_corners(f32 fov, f32 aspect_ratio, f32 z_near, f32 z_far) -> std::array<glm::vec3, 8> {
  auto tan_half_fov = glm::tan(glm::radians(fov) / 2.0f);
  auto a = glm::abs(z_near) * tan_half_fov;
  auto b = glm::abs(z_far) * tan_half_fov;

  return std::array<glm::vec3, 8>{
    glm::vec3(a * aspect_ratio, -a, z_near),  // bottom right
    glm::vec3(a * aspect_ratio, a, z_near),   // top right
    glm::vec3(-a * aspect_ratio, a, z_near),  // top left
    glm::vec3(-a * aspect_ratio, -a, z_near), // bottom left
    glm::vec3(b * aspect_ratio, -b, z_far),   // bottom right
    glm::vec3(b * aspect_ratio, b, z_far),    // top right
    glm::vec3(-b * aspect_ratio, b, z_far),   // top left
    glm::vec3(-b * aspect_ratio, -b, z_far),  // bottom left
  };
}

auto calculate_cascade_bounds(usize cascade_count, f32 nearest_bound, f32 maximum_shadow_distance)
  -> std::array<f32, MAX_DIRECTIONAL_SHADOW_CASCADES> {
  if (cascade_count == 1) {
    return {maximum_shadow_distance};
  }
  auto base = glm::pow(maximum_shadow_distance / nearest_bound, 1.0 / static_cast<f32>(cascade_count - 1));

  auto result = std::array<f32, MAX_DIRECTIONAL_SHADOW_CASCADES>();
  for (u32 i = 0; i < cascade_count; i++) {
    result[i] = nearest_bound * glm::pow(base, static_cast<f32>(i));
  }

  return result;
}

auto calculate_cascaded_shadow_matrices(
  GPU::DirectionalLight& light,
  std::span<GPU::DirectionalLightCascade> cascades,
  const LightComponent& light_comp,
  const CameraComponent& camera
) -> void {
  ZoneScoped;

  auto overlap_factor = 1.0 - light_comp.cascade_overlap_propotion;
  auto far_bounds = calculate_cascade_bounds(
    light.cascade_count,
    light_comp.first_cascade_far_bound,
    light_comp.maximum_shadow_distance
  );
  auto near_bounds = std::array<f32, MAX_DIRECTIONAL_SHADOW_CASCADES>();
  near_bounds[0] = light_comp.minimum_shadow_distance;
  for (u32 i = 1; i < light.cascade_count; i++) {
    near_bounds[i] = overlap_factor * far_bounds[i - 1];
  }

  auto forward = glm::normalize(light.direction);
  auto up = (glm::abs(glm::dot(forward, glm::vec3(0, 1, 0))) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
  auto right = glm::normalize(glm::cross(up, forward));
  up = glm::normalize(glm::cross(forward, right));

  auto world_from_light = glm::mat4(
    glm::vec4(right, 0.0f),
    glm::vec4(up, 0.0f),
    glm::vec4(forward, 0.0f),
    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)
  );
  auto light_to_world_inverse = glm::transpose(world_from_light);
  auto camera_to_world = camera.get_inv_view_matrix();

  for (u32 cascade_index = 0; cascade_index < light.cascade_count; ++cascade_index) {
    auto& cascade = cascades[cascade_index];
    auto split_near = near_bounds[cascade_index];
    auto split_far = far_bounds[cascade_index];
    auto corners = get_frustum_corners(camera.fov, camera.aspect, -split_near, -split_far);

    auto min = glm::vec3(std::numeric_limits<f32>::max());
    auto max = glm::vec3(std::numeric_limits<f32>::lowest());
    for (const auto& corner : corners) {
      auto world_corner = camera_to_world * glm::vec4(corner, 1.0f);
      auto light_view_corner = glm::vec3(light_to_world_inverse * world_corner);
      min = glm::min(min, light_view_corner);
      max = glm::max(max, light_view_corner);
    }

    auto body_diagonal = glm::length2(corners[0] - corners[6]);
    auto far_plane_diagonal = glm::length2(corners[4] - corners[6]);
    auto cascade_diameter = glm::ceil(glm::sqrt(glm::max(body_diagonal, far_plane_diagonal)));
    f32 cascade_texel_size = cascade_diameter / static_cast<f32>(light.cascade_size);

    glm::vec3 center = glm::vec3(
      glm::floor((min.x + max.x) * 0.5f / cascade_texel_size) * cascade_texel_size,
      glm::floor((min.y + max.y) * 0.5f / cascade_texel_size) * cascade_texel_size,
      max.z
    );

    auto cascade_from_world = glm::mat4(
      light_to_world_inverse[0],
      light_to_world_inverse[1],
      light_to_world_inverse[2],
      glm::vec4(-center, 1.0f)
    );

    auto z_extension = camera.far_clip * 0.5f;
    auto extended_min_z = min.z - z_extension;
    auto extended_max_z = max.z + z_extension;
    auto r = 1.0f / (extended_max_z - extended_min_z);
    auto clip_from_cascade = glm::mat4(
      glm::vec4(2.0 / cascade_diameter, 0.0, 0.0, 0.0),
      glm::vec4(0.0, 2.0 / cascade_diameter, 0.0, 0.0),
      glm::vec4(0.0, 0.0, r, 0.0),
      glm::vec4(0.0, 0.0, -extended_min_z * r, 1.0)
    );

    cascade.projection_view = clip_from_cascade * cascade_from_world;
    cascade.far_bound = split_far;
    cascade.texel_size = cascade_texel_size;
  }
}

RendererInstance::RendererInstance(Scene* owner_scene, Renderer& parent_renderer)
    : scene(owner_scene),
      renderer(parent_renderer) {

  auto& vk_context = App::get_vkcontext();
  render_queue_2d.init();

  spot_lights_buffer = vk_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    MAX_SPOT_LIGHTS * sizeof(GPU::SpotLight)
  );
  point_lights_buffer = vk_context.allocate_buffer_super(
    vuk::MemoryUsage::eGPUonly,
    MAX_POINT_LIGHTS * sizeof(GPU::PointLight)
  );

  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);
  before_callbacks.resize(stage_count);
  after_callbacks.resize(stage_count);

  rebuild_execution_order();
}

RendererInstance::~RendererInstance() {}

auto RendererInstance::add_stage_callback(this RendererInstance& self, RenderStageCallback callback) -> void {
  ZoneScoped;
  self.stage_callbacks.emplace_back(std::move(callback));
  self.rebuild_execution_order();
}

auto RendererInstance::clear_stages(this RendererInstance& self) -> void {
  ZoneScoped;
  self.stage_callbacks.clear();
}

auto RendererInstance::rebuild_execution_order(this RendererInstance& self) -> void {
  ZoneScoped;

  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);

  for (auto& vec : self.before_callbacks) {
    vec.clear();
  }
  for (auto& vec : self.after_callbacks) {
    vec.clear();
  }

  std::sort(
    self.stage_callbacks.begin(),
    self.stage_callbacks.end(),
    [](const RenderStageCallback& a, const RenderStageCallback& b) noexcept {
      return a.dependency.order < b.dependency.order;
    }
  );

  for (usize i = 0; i < self.stage_callbacks.size(); ++i) {
    const auto& callback = self.stage_callbacks[i];
    const usize stage_index = static_cast<usize>(callback.dependency.target_stage);

    if (stage_index >= stage_count) [[unlikely]] {
      continue;
    }

    if (!callback.callback) [[unlikely]] {
      continue;
    }

    if (callback.dependency.position == StagePosition::Before) {
      self.before_callbacks[stage_index].emplace_back(i);
    } else if (callback.dependency.position == StagePosition::After) {
      self.after_callbacks[stage_index].emplace_back(i);
    }
  }

  for (auto& vec : self.before_callbacks) {
    vec.shrink_to_fit();
  }
  for (auto& vec : self.after_callbacks) {
    vec.shrink_to_fit();
  }
}

auto RendererInstance::execute_stages_before(
  this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx
) -> void {
  ZoneScoped;

  const usize stage_index = static_cast<usize>(stage);
  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);

  if (stage_index >= stage_count) [[unlikely]] {
    return;
  }

  if (stage_index >= self.before_callbacks.size()) [[unlikely]] {
    return;
  }

  const auto& callbacks = self.before_callbacks[stage_index];

  for (const usize callback_idx : callbacks) {
    if (callback_idx >= self.stage_callbacks.size()) [[unlikely]] {
      continue;
    }

    const auto& callback = self.stage_callbacks[callback_idx];

    if (callback.callback) [[likely]] {
      callback.callback(ctx);
    }
  }
}

auto RendererInstance::execute_stages_after(
  this const RendererInstance& self, RenderStage stage, RenderStageContext& ctx
) -> void {
  ZoneScoped;

  const usize stage_index = static_cast<usize>(stage);
  constexpr usize stage_count = static_cast<usize>(RenderStage::Count);

  if (stage_index >= stage_count) [[unlikely]] {
    return;
  }

  if (stage_index >= self.after_callbacks.size()) [[unlikely]] {
    return;
  }

  const auto& callbacks = self.after_callbacks[stage_index];

  for (const usize callback_idx : callbacks) {
    if (callback_idx >= self.stage_callbacks.size()) [[unlikely]] {
      continue;
    }

    const auto& callback = self.stage_callbacks[callback_idx];

    if (callback.callback) [[likely]] {
      callback.callback(ctx);
    }
  }
}
auto RendererInstance::add_stage_before(
  this RendererInstance& self,
  RenderStage stage,
  const std::string& name,
  std::function<void(RenderStageContext&)> callback,
  int order
) -> void {
  StageDependency dep{.target_stage = stage, .position = StagePosition::Before, .order = order};
  self.add_stage_callback(RenderStageCallback{.callback = std::move(callback), .dependency = dep, .name = name});
}

auto RendererInstance::add_stage_after(
  this RendererInstance& self,
  RenderStage stage,
  const std::string& name,
  std::function<void(RenderStageContext&)> callback,
  int order
) -> void {
  StageDependency dep{.target_stage = stage, .position = StagePosition::After, .order = order};
  self.add_stage_callback(RenderStageCallback{.callback = std::move(callback), .dependency = dep, .name = name});
}

auto RendererInstance::render(this RendererInstance& self, const Renderer::RenderInfo& render_info)
  -> vuk::Value<vuk::ImageAttachment> {
  ZoneScoped;

  OX_DEFER(&) { self.clear_stages(); };

  self.viewport_size = {render_info.extent.width, render_info.extent.height};
  self.viewport_offset = render_info.viewport_offset;

  auto& bindless_set = self.renderer.vk_context->get_descriptor_set();

  self.camera_data.resolution = {render_info.extent.width, render_info.extent.height};
  self.prepared_frame.camera_buffer = self.renderer.vk_context->scratch_buffer(self.camera_data);

  self.render_queue_2d.update();
  self.render_queue_2d.sort();
  auto vertex_buffer_2d = self.renderer.vk_context->scratch_buffer_span(std::span(self.render_queue_2d.sprite_data));

  auto prepare_lights_pass = vuk::make_pass(
    "prepare lights",
    [](
      vuk::CommandBuffer&,
      VUK_BA(vuk::eMemoryRead) lights_,
      VUK_BA(vuk::eMemoryRead) atmos,
      VUK_BA(vuk::eMemoryRead) directional_light_,
      VUK_BA(vuk::eMemoryRead) directional_light_cascades_,
      VUK_BA(vuk::eMemoryRead) point_lights,
      VUK_BA(vuk::eMemoryRead) spot_lights
    ) {
      return std::make_tuple(
        lights_,
        atmos,
        directional_light_,
        directional_light_cascades_,
        point_lights,
        spot_lights
      );
    }
  );

  std::tie(
    self.prepared_frame.lights_buffer,
    self.prepared_frame.atmosphere_buffer,
    self.prepared_frame.directional_light_buffer,
    self.prepared_frame.directional_light_cascades_buffer,
    self.prepared_frame.point_lights_buffer,
    self.prepared_frame.spot_lights_buffer
  ) =
    prepare_lights_pass(
      std::move(self.prepared_frame.lights_buffer),
      std::move(self.prepared_frame.atmosphere_buffer),
      std::move(self.prepared_frame.directional_light_buffer),
      std::move(self.prepared_frame.directional_light_cascades_buffer),
      std::move(self.prepared_frame.point_lights_buffer),
      std::move(self.prepared_frame.spot_lights_buffer)
    );

  if (static_cast<bool>(RendererCVar::cvar_bloom_enable.get()))
    self.gpu_scene_flags |= GPU::SceneFlags::HasBloom;
  if (static_cast<bool>(RendererCVar::cvar_fxaa_enable.get()))
    self.gpu_scene_flags |= GPU::SceneFlags::HasFXAA;
  if (static_cast<bool>(RendererCVar::cvar_vbgtao_enable.get()))
    self.gpu_scene_flags |= GPU::SceneFlags::HasGTAO;
  if (static_cast<bool>(RendererCVar::cvar_contact_shadows.get()))
    self.gpu_scene_flags |= GPU::SceneFlags::HasContactShadows;

  const auto debug_view = static_cast<GPU::DebugView>(RendererCVar::cvar_debug_view.get());
  const f32 debug_heatmap_scale = 5.0;
  const auto debugging = debug_view != GPU::DebugView::None;

  auto final_attachment = vuk::declare_ia(
    "final_attachment",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .extent = render_info.extent,
     .format = vuk::Format::eB10G11R11UfloatPack32,
     .sample_count = vuk::Samples::e1,
     .level_count = 1,
     .layer_count = 1}
  );
  final_attachment = vuk::clear_image(std::move(final_attachment), vuk::Black<float>);

  auto depth_attachment = vuk::declare_ia(
    "depth_image",
    {.usage = vuk::ImageUsageFlagBits::eDepthStencilAttachment | vuk::ImageUsageFlagBits::eSampled,
     .extent = render_info.extent,
     .format = vuk::Format::eD32Sfloat,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = 1,
     .layer_count = 1}
  );
  depth_attachment = vuk::clear_image(std::move(depth_attachment), vuk::DepthZero);

  auto hiz_extent = vuk::Extent3D{
    .width = std::bit_ceil((depth_attachment->extent.width + 1) >> 1),
    .height = std::bit_ceil((depth_attachment->extent.height + 1) >> 1),
    .depth = 1,
  };

  auto hiz_attachment = vuk::declare_ia(
    "hiz",
    {.usage = vuk::ImageUsageFlagBits::eStorage | vuk::ImageUsageFlagBits::eSampled,
     .extent = hiz_extent,
     .format = vuk::Format::eR32Sfloat,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = Texture::get_mip_count(hiz_extent),
     .layer_count = 1}
  );
  hiz_attachment = vuk::clear_image(std::move(hiz_attachment), vuk::DepthZero);

  auto sky_transmittance_lut_attachment = self.renderer.sky_transmittance_lut_view.acquire(
    "sky_transmittance_lut",
    vuk::Access::eComputeSampled
  );
  auto sky_multiscatter_lut_attachment = self.renderer.sky_multiscatter_lut_view.acquire(
    "sky_multiscatter_lut",
    vuk::Access::eComputeSampled
  );
  auto sky_view_lut_attachment = vuk::declare_ia(
    "sky_view_lut",
    {.image_type = vuk::ImageType::e2D,
     .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .extent = self.sky_view_lut_extent,
     .format = vuk::Format::eR16G16B16A16Sfloat,
     .sample_count = vuk::Samples::e1,
     .view_type = vuk::ImageViewType::e2D,
     .level_count = 1,
     .layer_count = 1}
  );

  auto sky_aerial_perspective_attachment = vuk::declare_ia(
    "sky aerial perspective",
    {.image_type = vuk::ImageType::e3D,
     .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .extent = self.sky_aerial_perspective_lut_extent,
     .sample_count = vuk::Samples::e1,
     .view_type = vuk::ImageViewType::e3D,
     .level_count = 1,
     .layer_count = 1}
  );
  sky_aerial_perspective_attachment.same_format_as(sky_view_lut_attachment);

  auto hilbert_noise_lut_attachment = self.renderer.hilbert_noise_lut.acquire("hilbert noise", vuk::eComputeSampled);

  auto visbuffer_attachment = vuk::declare_ia(
    "visbuffer",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR32Uint,
     .sample_count = vuk::SampleCountFlagBits::e1}
  );
  visbuffer_attachment.same_shape_as(final_attachment);

  auto overdraw_attachment = vuk::declare_ia(
    "overdraw",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .sample_count = vuk::SampleCountFlagBits::e1}
  );
  overdraw_attachment.similar_to(visbuffer_attachment);

  auto vis_clear_pass = vuk::make_pass(
    "vis clear",
    [](
      vuk::CommandBuffer& cmd_list, //
      VUK_IA(vuk::eComputeWrite) visbuffer,
      VUK_IA(vuk::eComputeWrite) overdraw
    ) {
      cmd_list //
        .bind_compute_pipeline("visbuffer_clear")
        .bind_image(0, 0, visbuffer)
        .bind_image(0, 1, overdraw)
        .push_constants(
          vuk::ShaderStageFlagBits::eCompute,
          0,
          PushConstants(glm::uvec2(visbuffer->extent.width, visbuffer->extent.height))
        )
        .dispatch_invocations_per_pixel(visbuffer);

      return std::make_tuple(visbuffer, overdraw);
    }
  );
  std::tie(visbuffer_attachment, overdraw_attachment) = vis_clear_pass(
    std::move(visbuffer_attachment),
    std::move(overdraw_attachment)
  );

  auto directional_shadows_enabled = self.directional_light_cast_shadows;
  auto directional_light_shadowmap_size = max(self.directional_light.cascade_size * directional_shadows_enabled, 1_u32);
  auto directional_light_shadowmap_attachment = vuk::declare_ia(
    "directional light shadowmap",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eDepthStencilAttachment,
     .extent = vuk::Extent3D{directional_light_shadowmap_size, directional_light_shadowmap_size, 1},
     .format = vuk::Format::eD32Sfloat,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = 1,
     .layer_count = max(self.directional_light.cascade_count, 2_u32)}
  );
  directional_light_shadowmap_attachment = vuk::clear_image(
    std::move(directional_light_shadowmap_attachment),
    vuk::DepthZero
  );

  auto contact_shadows_attachment = vuk::declare_ia(
    "contact shadows",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eR32Sfloat,
     .sample_count = vuk::SampleCountFlagBits::e1}
  );
  contact_shadows_attachment.same_shape_as(final_attachment);
  contact_shadows_attachment = vuk::clear_image(std::move(contact_shadows_attachment), vuk::Black<f32>);

  auto albedo_attachment = vuk::declare_ia(
    "albedo",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR8G8B8A8Srgb,
     .sample_count = vuk::Samples::e1}
  );
  albedo_attachment.same_shape_as(visbuffer_attachment);
  albedo_attachment = vuk::clear_image(std::move(albedo_attachment), vuk::Black<f32>);

  auto normal_attachment = vuk::declare_ia(
    "normal",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR16G16B16A16Sfloat,
     .sample_count = vuk::Samples::e1}
  );
  normal_attachment.same_shape_as(visbuffer_attachment);
  normal_attachment = vuk::clear_image(std::move(normal_attachment), vuk::Black<f32>);

  auto emissive_attachment = vuk::declare_ia(
    "emissive",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eB10G11R11UfloatPack32,
     .sample_count = vuk::Samples::e1}
  );
  emissive_attachment.same_shape_as(visbuffer_attachment);
  emissive_attachment = vuk::clear_image(std::move(emissive_attachment), vuk::Black<f32>);

  auto metallic_roughness_occlusion_attachment = vuk::declare_ia(
    "metallic roughness occlusion",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
     .format = vuk::Format::eR8G8B8A8Unorm,
     .sample_count = vuk::Samples::e1}
  );
  metallic_roughness_occlusion_attachment.same_shape_as(visbuffer_attachment);
  metallic_roughness_occlusion_attachment = vuk::clear_image(
    std::move(metallic_roughness_occlusion_attachment),
    vuk::Black<f32>
  );

  auto vbgtao_occlusion_attachment = vuk::declare_ia(
    "vbgtao occlusion",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eR16Sfloat,
     .sample_count = vuk::Samples::e1,
     .view_type = vuk::ImageViewType::e2D,
     .level_count = 1,
     .layer_count = 1}
  );
  vbgtao_occlusion_attachment.same_extent_as(depth_attachment);
  vbgtao_occlusion_attachment = vuk::clear_image(std::move(vbgtao_occlusion_attachment), vuk::White<f32>);

  auto vbgtao_depth_differences_attachment = vuk::declare_ia(
    "vbgtao depth differences",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eR32Uint,
     .sample_count = vuk::Samples::e1}
  );
  vbgtao_depth_differences_attachment.same_shape_as(vbgtao_occlusion_attachment);
  vbgtao_depth_differences_attachment = vuk::clear_image(
    std::move(vbgtao_depth_differences_attachment),
    vuk::Black<f32>
  );

  const f32 bloom_threshold = RendererCVar::cvar_bloom_threshold.get();
  const f32 bloom_clamp = RendererCVar::cvar_bloom_clamp.get();
  const u32 bloom_quality_level = static_cast<u32>(RendererCVar::cvar_bloom_quality_level.get());
  u32 bloom_mip_count = 8;
  switch (bloom_quality_level) {
    case 0: {
      bloom_mip_count = 4;
      break;
    }
    case 1: {
      bloom_mip_count = 5;
      break;
    }
    case 2: {
      bloom_mip_count = 6;
      break;
    }
    case 3: {
      bloom_mip_count = 8;
      break;
    }
  }

  auto bloom_upsampled_attachment = vuk::declare_ia(
    "bloom upsampled",
    {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eStorage,
     .format = vuk::Format::eB10G11R11UfloatPack32,
     .sample_count = vuk::SampleCountFlagBits::e1,
     .level_count = bloom_mip_count - 1,
     .layer_count = 1}
  );
  bloom_upsampled_attachment.same_extent_as(final_attachment);
  bloom_upsampled_attachment = vuk::clear_image(std::move(bloom_upsampled_attachment), vuk::Black<float>);

  // --- 3D Pass ---
  if (self.prepared_frame.mesh_instance_count > 0) {
    if (self.directional_light_cast_shadows) {
      auto directional_light_resolution = glm::vec2(directional_light_shadowmap_size, directional_light_shadowmap_size);
      for (u32 cascade_index = 0; cascade_index < self.directional_light.cascade_count; cascade_index++) {
        auto current_cascade_attachment = directional_light_shadowmap_attachment.layer(cascade_index);
        auto& current_cascade = self.directional_light_cascades[cascade_index];

        auto shadow_geometry_context = ShadowGeometryContext{
          .shadowmap_attachment = std::move(current_cascade_attachment),
        };
        self.cull_for_shadowmap(shadow_geometry_context, current_cascade.projection_view);
        self.draw_for_shadowmap(shadow_geometry_context, current_cascade.projection_view, cascade_index);
      }

      auto contact_shadows_pass = vuk::make_pass(
        "contact_shadows",
        [sun_dir = self.directional_light.direction](
          vuk::CommandBuffer& cmd_list,
          VUK_IA(vuk::eComputeRW) result,
          VUK_IA(vuk::eComputeSampled) src_depth,
          VUK_BA(vuk::eComputeRead) camera
        ) {
          const u32 steps = static_cast<u32>(RendererCVar::cvar_contact_shadows_steps.get());
          const f32 thickness = RendererCVar::cvar_contact_shadows_thickness.get();
          const f32 length = RendererCVar::cvar_contact_shadows_length.get();

          cmd_list //
            .bind_compute_pipeline("contact_shadows")
            .bind_image(0, 0, src_depth)
            .bind_image(0, 1, result)
            .bind_buffer(0, 2, camera)
            .bind_sampler(0, 3, vuk::NearestSamplerClamped)
            .bind_sampler(0, 4, vuk::LinearSamplerClamped)
            .push_constants(vuk::ShaderStageFlagBits::eCompute, 0, PushConstants(sun_dir, steps, thickness, length))
            .dispatch_invocations_per_pixel(result);

          return std::make_tuple(result, src_depth, camera);
        }
      );

      std::tie(contact_shadows_attachment, depth_attachment, self.prepared_frame.camera_buffer) = contact_shadows_pass(
        contact_shadows_attachment,
        depth_attachment,
        self.prepared_frame.camera_buffer
      );
    }

    auto main_geometry_context = MainGeometryContext{
      .bindless_set = &bindless_set,
      .depth_attachment = std::move(depth_attachment),
      .hiz_attachment = std::move(hiz_attachment),
      .visbuffer_attachment = std::move(visbuffer_attachment),
      .overdraw_attachment = std::move(overdraw_attachment),
      .albedo_attachment = std::move(albedo_attachment),
      .normal_attachment = std::move(normal_attachment),
      .emissive_attachment = std::move(emissive_attachment),
      .metallic_roughness_occlusion_attachment = std::move(metallic_roughness_occlusion_attachment),
    };

    main_geometry_context.late = false;
    self.cull_for_visbuffer(main_geometry_context);
    self.draw_for_visbuffer(main_geometry_context);

    self.generate_hiz(main_geometry_context);

    main_geometry_context.late = true;
    self.cull_for_visbuffer(main_geometry_context);
    self.draw_for_visbuffer(main_geometry_context);

    {
      RenderStageContext ctx(self, self.shared_resources, RenderStage::VisBufferEncode, *self.renderer.vk_context);
      ctx.set_viewport_size(self.viewport_size)
        .set_image_resource("visbuffer_attachment", std::move(main_geometry_context.visbuffer_attachment))
        .set_image_resource("depth_attachment", std::move(main_geometry_context.depth_attachment))
        .set_buffer_resource("meshlet_instances_buffer", std::move(self.prepared_frame.meshlet_instances_buffer))
        .set_buffer_resource("mesh_instances_buffer", std::move(self.prepared_frame.mesh_instances_buffer));

      self.execute_stages_after(RenderStage::VisBufferEncode, ctx);

      main_geometry_context.visbuffer_attachment = ctx.get_image_resource("visbuffer_attachment");
      main_geometry_context.depth_attachment = ctx.get_image_resource("depth_attachment");
      self.prepared_frame.meshlet_instances_buffer = ctx.get_buffer_resource("meshlet_instances_buffer");
      self.prepared_frame.mesh_instances_buffer = ctx.get_buffer_resource("mesh_instances_buffer");
    }

    self.decode_visbuffer(main_geometry_context);

    visbuffer_attachment = std::move(main_geometry_context.visbuffer_attachment);
    depth_attachment = std::move(main_geometry_context.depth_attachment);
    overdraw_attachment = std::move(main_geometry_context.overdraw_attachment);
    albedo_attachment = std::move(main_geometry_context.albedo_attachment);
    normal_attachment = std::move(main_geometry_context.normal_attachment);
    emissive_attachment = std::move(main_geometry_context.emissive_attachment);
    metallic_roughness_occlusion_attachment = std::move(main_geometry_context.metallic_roughness_occlusion_attachment);
  }

  if (self.gpu_scene_flags & GPU::SceneFlags::HasAtmosphere &&
      self.gpu_scene_flags & GPU::SceneFlags::HasDirectionalLight) {
    auto atmos_context = AtmosphereContext{
      .sky_transmittance_lut_attachment = std::move(sky_transmittance_lut_attachment),
      .sky_multiscatter_lut_attachment = std::move(sky_multiscatter_lut_attachment),
      .sky_view_lut_attachment = std::move(sky_view_lut_attachment),
      .sky_aerial_perspective_lut_attachment = std::move(sky_aerial_perspective_attachment),
    };
    self.draw_atmosphere(atmos_context);

    sky_transmittance_lut_attachment = std::move(atmos_context.sky_transmittance_lut_attachment);
    sky_multiscatter_lut_attachment = std::move(atmos_context.sky_multiscatter_lut_attachment);
    sky_view_lut_attachment = std::move(atmos_context.sky_view_lut_attachment);
    sky_aerial_perspective_attachment = std::move(atmos_context.sky_aerial_perspective_lut_attachment);
  }

  if (self.gpu_scene_flags & GPU::SceneFlags::HasGTAO) {
    auto ao_context = AmbientOcclusionContext{
      .noise_attachment = std::move(hilbert_noise_lut_attachment),
      .normal_attachment = std::move(normal_attachment),
      .depth_attachment = std::move(depth_attachment),
      .depth_differences_attachment = std::move(vbgtao_depth_differences_attachment),
      .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
    };
    self.generate_ambient_occlusion(ao_context);

    hilbert_noise_lut_attachment = std::move(ao_context.noise_attachment);
    normal_attachment = std::move(ao_context.normal_attachment);
    depth_attachment = std::move(ao_context.depth_attachment);
    vbgtao_depth_differences_attachment = std::move(ao_context.depth_differences_attachment);
    vbgtao_occlusion_attachment = std::move(ao_context.ambient_occlusion_attachment);
  }

  auto pbr_context = PBRContext{
    .bindless_set = &bindless_set,
    .sky_transmittance_lut_attachment = std::move(sky_transmittance_lut_attachment),
    .sky_aerial_perspective_lut_attachment = std::move(sky_aerial_perspective_attachment),
    .sky_view_lut_attachment = std::move(sky_view_lut_attachment),
    .depth_attachment = std::move(depth_attachment),
    .albedo_attachment = std::move(albedo_attachment),
    .normal_attachment = std::move(normal_attachment),
    .emissive_attachment = std::move(emissive_attachment),
    .metallic_roughness_occlusion_attachment = std::move(metallic_roughness_occlusion_attachment),
    .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
    .contact_shadows_attachment = std::move(contact_shadows_attachment),
    .directional_shadowmap_attachment = std::move(directional_light_shadowmap_attachment),
  };
  final_attachment = self.apply_pbr(pbr_context, std::move(final_attachment));
  depth_attachment = std::move(pbr_context.depth_attachment);
  albedo_attachment = std::move(pbr_context.albedo_attachment);
  normal_attachment = std::move(pbr_context.normal_attachment);
  emissive_attachment = std::move(pbr_context.emissive_attachment);
  metallic_roughness_occlusion_attachment = std::move(pbr_context.metallic_roughness_occlusion_attachment);
  vbgtao_occlusion_attachment = std::move(pbr_context.ambient_occlusion_attachment);

  // --- 2D Pass ---
  if (!self.render_queue_2d.sprite_data.empty()) {
    // WARN: rq2d is copied each frame (it needs to be copied)
    auto forward_2d_pass = vuk::make_pass(
      "2d_forward_pass",
      [rq2d = self.render_queue_2d, &descriptor_set = bindless_set](
        vuk::CommandBuffer& command_buffer,
        VUK_IA(vuk::eColorWrite) target,
        VUK_IA(vuk::eDepthStencilRW) depth,
        VUK_BA(vuk::eAttributeRead) vertex_buffer,
        VUK_BA(vuk::eVertexRead) materials,
        VUK_BA(vuk::eVertexRead) camera,
        VUK_BA(vuk::eVertexRead) transforms_
      ) {
        const auto vertex_pack_2d = vuk::Packed{
          vuk::Format::eR32Uint, // 4 material_id
          vuk::Format::eR32Uint, // 4 flags
          vuk::Format::eR32Uint, // 4 transforms_id
        };

        for (const auto& batch : rq2d.batches) {
          if (batch.count < 1)
            continue;

          command_buffer.bind_graphics_pipeline(batch.pipeline_name)
            .set_depth_stencil(
              vuk::PipelineDepthStencilStateCreateInfo{
                .depthTestEnable = true,
                .depthWriteEnable = true,
                .depthCompareOp = vuk::CompareOp::eGreaterOrEqual,
              }
            )
            .set_dynamic_state(vuk::DynamicStateFlagBits::eScissor | vuk::DynamicStateFlagBits::eViewport)
            .set_viewport(0, vuk::Rect2D::framebuffer())
            .set_scissor(0, vuk::Rect2D::framebuffer())
            .broadcast_color_blend(vuk::BlendPreset::eAlphaBlend)
            .set_rasterization({.cullMode = vuk::CullModeFlagBits::eNone})
            .bind_vertex_buffer(0, vertex_buffer, 0, vertex_pack_2d, vuk::VertexInputRate::eInstance)
            .push_constants(
              vuk::ShaderStageFlagBits::eVertex | vuk::ShaderStageFlagBits::eFragment,
              0,
              PushConstants(materials->device_address, camera->device_address, transforms_->device_address)
            )
            .bind_persistent(1, descriptor_set)
            .draw(6, batch.count, 0, batch.offset);
        }

        return std::make_tuple(target, depth, camera, vertex_buffer, materials, transforms_);
      }
    );

    std::tie(
      final_attachment,
      depth_attachment,
      self.prepared_frame.camera_buffer,
      vertex_buffer_2d,
      self.prepared_frame.materials_buffer,
      self.prepared_frame.transforms_buffer
    ) =
      forward_2d_pass(
        std::move(final_attachment),
        std::move(depth_attachment),
        std::move(vertex_buffer_2d),
        std::move(self.prepared_frame.materials_buffer),
        std::move(self.prepared_frame.camera_buffer),
        std::move(self.prepared_frame.transforms_buffer)
      );
  }

  // --- FXAA Pass ---
  if (self.gpu_scene_flags & GPU::SceneFlags::HasFXAA) {
    auto fxaa_attachment = vuk::declare_ia(
      "fxaa_attachment",
      {.usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eColorAttachment,
       .sample_count = vuk::Samples::e1}
    );
    fxaa_attachment.same_shape_as(final_attachment);
    fxaa_attachment.same_format_as(final_attachment);
    fxaa_attachment = vuk::clear_image(std::move(fxaa_attachment), vuk::Black<f32>);

    auto fxaa_pass = vuk::make_pass(
      "fxaa",
      [](vuk::CommandBuffer& cmd_list, VUK_IA(vuk::eColorWrite) dst, VUK_IA(vuk::eFragmentSampled) src) {
        const glm::vec2 inverse_screen_size = 1.f / glm::vec2(src->extent.width, src->extent.height);
        cmd_list.bind_graphics_pipeline("fxaa")
          .set_rasterization({})
          .set_color_blend(dst, {})
          .set_dynamic_state(vuk::DynamicStateFlagBits::eViewport | vuk::DynamicStateFlagBits::eScissor)
          .set_viewport(0, vuk::Rect2D::framebuffer())
          .set_scissor(0, vuk::Rect2D::framebuffer())
          .bind_image(0, 0, src)
          .bind_sampler(0, 1, vuk::LinearSamplerClamped)
          .push_constants(vuk::ShaderStageFlagBits::eFragment, 0, PushConstants(inverse_screen_size))
          .draw(3, 1, 0, 0);
        return std::make_tuple(dst, src);
      }
    );

    std::tie(final_attachment, fxaa_attachment) = fxaa_pass(fxaa_attachment, final_attachment);
  }

  /// POST PROCESSING
  auto post_process_context = PostProcessContext{
    .delta_time = static_cast<f32>(App::get_timestep().get_millis()) * 0.001f,
    .final_attachment = std::move(final_attachment),
    .bloom_upsampled_attachment = std::move(bloom_upsampled_attachment),
  };

  if (self.gpu_scene_flags & GPU::SceneFlags::HasEyeAdaptation) {
    self.apply_eye_adaptation(post_process_context);
  }

  if (self.gpu_scene_flags & GPU::SceneFlags::HasBloom) {
    self.apply_bloom(post_process_context, bloom_threshold, bloom_clamp, bloom_mip_count);
  }

  auto result_attachment = self.apply_tonemap(post_process_context, render_info.format);

  {
    RenderStageContext ctx(self, self.shared_resources, RenderStage::PostProcessing, *self.renderer.vk_context);
    ctx.set_viewport_size(self.viewport_size)
      .set_buffer_resource("camera_buffer", std::move(self.prepared_frame.camera_buffer))
      .set_image_resource("depth_attachment", std::move(depth_attachment))
      .set_image_resource("result_attachment", std::move(result_attachment));

    self.execute_stages_after(RenderStage::PostProcessing, ctx);

    self.prepared_frame.camera_buffer = ctx.get_buffer_resource("camera_buffer");
    depth_attachment = ctx.get_image_resource("depth_attachment");
    result_attachment = ctx.get_image_resource("result_attachment");
  }

  if (debugging) {
    auto debug_context = DebugContext{
      .overdraw_heatmap_scale = debug_heatmap_scale,
      .debug_view = debug_view,
      .visbuffer_attachment = std::move(visbuffer_attachment),
      .depth_attachment = std::move(depth_attachment),
      .overdraw_attachment = std::move(overdraw_attachment),
      .albedo_attachment = std::move(albedo_attachment),
      .normal_attachment = std::move(normal_attachment),
      .emissive_attachment = std::move(emissive_attachment),
      .metallic_roughness_occlusion_attachment = std::move(metallic_roughness_occlusion_attachment),
      .ambient_occlusion_attachment = std::move(vbgtao_occlusion_attachment),
    };
    return self.apply_debug_view(debug_context, render_info.extent);
  }

  return result_attachment;
}

auto RendererInstance::update(this RendererInstance& self, RendererInstanceUpdateInfo& info) -> void {
  ZoneScoped;

  auto& asset_man = App::mod<AssetManager>();
  auto& vk_context = *self.renderer.vk_context;

  self.gpu_scene_flags = {};

  CameraComponent current_camera = {};
  CameraComponent frozen_camera = {};
  const auto freeze_culling = static_cast<bool>(RendererCVar::cvar_freeze_culling_frustum.get());

  self.scene->world
    .query_builder<const TransformComponent, const CameraComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const CameraComponent& c) {
      if (freeze_culling && !self.saved_camera) {
        self.saved_camera = true;
        frozen_camera = current_camera;
      } else if (!freeze_culling && self.saved_camera) {
        self.saved_camera = false;
      }

      if (static_cast<bool>(RendererCVar::cvar_freeze_culling_frustum.get()) &&
          static_cast<bool>(RendererCVar::cvar_draw_camera_frustum.get())) {
        const auto proj = frozen_camera.get_projection_matrix() * frozen_camera.get_view_matrix();
        DebugRenderer::draw_frustum(proj, glm::vec4(0, 1, 0, 1), frozen_camera.near_clip, frozen_camera.far_clip);
      }

      current_camera = c;
    });

  CameraComponent cam = freeze_culling ? frozen_camera : current_camera;

  self.camera_data = GPU::CameraData{
    .position = glm::vec4(cam.position, 0.0f),
    .projection = cam.get_projection_matrix(),
    .inv_projection = cam.get_inv_projection_matrix(),
    .view = cam.get_view_matrix(),
    .inv_view = cam.get_inv_view_matrix(),
    .projection_view = cam.get_projection_matrix() * cam.get_view_matrix(),
    .inv_projection_view = cam.get_inverse_projection_view(),
    .previous_projection = self.previous_camera_data.projection,
    .previous_inv_projection = self.previous_camera_data.inv_projection,
    .previous_view = self.previous_camera_data.view,
    .previous_inv_view = self.previous_camera_data.inv_view,
    .previous_projection_view = self.previous_camera_data.projection_view,
    .previous_inv_projection_view = self.previous_camera_data.inv_projection_view,
    .temporalaa_jitter = cam.jitter,
    .temporalaa_jitter_prev = self.previous_camera_data.temporalaa_jitter_prev,
    .near_clip = cam.near_clip,
    .far_clip = cam.far_clip,
    .fov = cam.fov,
    .output_index = 0,
    .acceptable_lod_error = 2.0f,
  };

  self.previous_camera_data = self.camera_data;

  math::calc_frustum_planes(self.camera_data.projection_view, self.camera_data.frustum_planes);

  std::vector<GPU::PointLight> point_lights = {};
  std::vector<GPU::SpotLight> spot_lights = {};

  self.scene->world
    .query_builder<const TransformComponent, const LightComponent>() //
    .build()
    .each([&self,
           cam,
           &point_lights,
           &spot_lights,
           current_camera](flecs::entity e, const TransformComponent& tc, const LightComponent& lc) {
      if (lc.type == LightComponent::LightType::Directional) {
        self.gpu_scene_flags |= GPU::SceneFlags::HasDirectionalLight;
        self.directional_light.color = lc.color;
        self.directional_light.intensity = lc.intensity;
        self.directional_light.direction.x = glm::cos(tc.rotation.x) * glm::sin(tc.rotation.y);
        self.directional_light.direction.y = glm::sin(tc.rotation.x) * glm::sin(tc.rotation.y);
        self.directional_light.direction.z = glm::cos(tc.rotation.y);
        self.directional_light.cascade_count = ox::min(
          lc.cascade_count,
          static_cast<u32>(MAX_DIRECTIONAL_SHADOW_CASCADES)
        );
        self.directional_light.cascade_size = lc.shadow_map_res;
        self.directional_light.cascades_overlap_proportion = lc.cascade_overlap_propotion;
        self.directional_light.depth_bias = lc.depth_bias;
        self.directional_light.normal_bias = lc.normal_bias;

        self.directional_light_cast_shadows = lc.cast_shadows;

        if (lc.cast_shadows) {
          calculate_cascaded_shadow_matrices(
            self.directional_light,
            self.directional_light_cascades,
            lc,
            current_camera
          );
        }
      } else if (lc.type == LightComponent::LightType::Point) {
        const glm::vec3 world_pos = Scene::get_world_position(e);
        point_lights.emplace_back(
          GPU::PointLight{
            .position = world_pos,
            .color = lc.color,
            .intensity = lc.intensity,
            .cutoff = lc.radius,
          }
        );
      } else if (lc.type == LightComponent::LightType::Spot) {
        const glm::vec3 direction = {
          glm::cos(tc.rotation.x) * glm::sin(tc.rotation.y),
          -glm::sin(tc.rotation.x),
          glm::cos(tc.rotation.x) * glm::cos(tc.rotation.y),
        };

        const glm::vec3 world_pos = Scene::get_world_position(e);
        spot_lights.emplace_back(
          GPU::SpotLight{
            .position = world_pos,
            .direction = glm::normalize(direction),
            .color = lc.color,
            .intensity = lc.intensity,
            .cutoff = lc.radius,
            .inner_cone_angle = lc.inner_cone_angle,
            .outer_cone_angle = lc.outer_cone_angle,
          }
        );
      }

      if (const auto* atmos_info = e.try_get<AtmosphereComponent>()) {
        self.gpu_scene_flags |= GPU::SceneFlags::HasAtmosphere;

        self.atmosphere.rayleigh_scatter = atmos_info->rayleigh_scattering * 1e-3f;
        self.atmosphere.rayleigh_density = atmos_info->rayleigh_density;
        self.atmosphere.mie_scatter = atmos_info->mie_scattering * 1e-3f;
        self.atmosphere.mie_density = atmos_info->mie_density;
        self.atmosphere.mie_extinction = atmos_info->mie_extinction * 1e-3f;
        self.atmosphere.mie_asymmetry = atmos_info->mie_asymmetry;
        self.atmosphere.ozone_absorption = atmos_info->ozone_absorption * 1e-3f;
        self.atmosphere.ozone_height = atmos_info->ozone_height;
        self.atmosphere.ozone_thickness = atmos_info->ozone_thickness;
        self.atmosphere.aerial_perspective_start_km = atmos_info->aerial_perspective_start_km;
        self.atmosphere.aerial_perspective_exposure = atmos_info->aerial_perspective_exposure;
        self.atmosphere.sky_view_lut_size = self.sky_view_lut_extent;
        self.atmosphere.aerial_perspective_lut_size = self.sky_aerial_perspective_lut_extent;
        self.atmosphere.transmittance_lut_size = self.renderer.sky_transmittance_lut_view.get_extent();
        self.atmosphere.multiscattering_lut_size = self.renderer.sky_multiscatter_lut_view.get_extent();

        f32 eye_altitude = cam.position.y * GPU::CAMERA_SCALE_UNIT;
        eye_altitude += self.atmosphere.planet_radius + GPU::PLANET_RADIUS_OFFSET;
        self.atmosphere.eye_position = glm::vec3(0.0f, eye_altitude, 0.0f);
      }
    });

  self.scene->world
    .query_builder<const TransformComponent, const SpriteComponent>() //
    .build()
    .each([&asset_man,
           &s = self.scene,
           &cam,
           &rq2d = self.render_queue_2d](flecs::entity e, const TransformComponent& tc, const SpriteComponent& comp) {
      const auto distance = glm::distance(glm::vec3(0.f, 0.f, cam.position.z), glm::vec3(0.f, 0.f, tc.position.z));
      if (auto* material = asset_man.get_asset(comp.material)) {
        if (auto transform_id = s->get_entity_transform_id(e)) {
          rq2d.add(
            comp,
            tc.position.y,
            SlotMap_decode_id(*transform_id).index,
            SlotMap_decode_id(material->material_id).index,
            distance
          );
        } else {
          OX_LOG_WARN("No registered transform for sprite entity: {}", e.name().c_str());
        }
      }
    });

  self.scene->world
    .query_builder<const TransformComponent, const ParticleComponent>() //
    .build()
    .each([&asset_man,
           &s = self.scene,
           &cam,
           &rq2d = self.render_queue_2d](flecs::entity e, const TransformComponent& tc, const ParticleComponent& comp) {
      if (comp.life_remaining <= 0.0f)
        return;

      const auto distance = glm::distance(glm::vec3(0.f, 0.f, cam.position.z), glm::vec3(0.f, 0.f, tc.position.z));

      auto particle_system_component = e.parent().try_get<ParticleSystemComponent>();
      if (particle_system_component) {
        if (auto* material = asset_man.get_asset(particle_system_component->material)) {
          if (auto transform_id = s->get_entity_transform_id(e)) {
            SpriteComponent sprite_comp = {.sort_y = true};

            rq2d.add(
              sprite_comp,
              tc.position.y,
              SlotMap_decode_id(*transform_id).index,
              SlotMap_decode_id(material->material_id).index,
              distance
            );
          } else {
            OX_LOG_WARN("No registered transform for sprite entity: {}", e.name().c_str());
          }
        }
      }
    });

  self.scene->world
    .query_builder<const AutoExposureComponent>() //
    .build()
    .each([&self](flecs::entity e, const AutoExposureComponent& c) {
      self.gpu_scene_flags |= GPU::SceneFlags::HasEyeAdaptation;
      self.eye_adaptation.max_exposure = c.max_exposure;
      self.eye_adaptation.min_exposure = c.min_exposure;
      self.eye_adaptation.adaptation_speed = c.adaptation_speed;
      self.eye_adaptation.ev100_bias = c.ev100_bias;
    });

  self.scene->world
    .query_builder<const TransformComponent, const VignetteComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const VignetteComponent& c) {
      self.post_proces_settings.vignette_amount = c.amount;

      self.gpu_scene_flags |= GPU::SceneFlags::HasVignette;
    });

  self.scene->world
    .query_builder<const TransformComponent, const ChromaticAberrationComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const ChromaticAberrationComponent& c) {
      self.post_proces_settings.chromatic_aberration_amount = c.amount;

      self.gpu_scene_flags |= GPU::SceneFlags::HasChromaticAberration;
    });

  self.scene->world
    .query_builder<const TransformComponent, const FilmGrainComponent>() //
    .build()
    .each([&](flecs::entity e, const TransformComponent& tc, const FilmGrainComponent& c) {
      self.post_proces_settings.film_grain_amount = c.amount;
      self.post_proces_settings.film_grain_scale = c.scale;
      self.post_proces_settings.film_grain_seed = vk_context.num_frames % 16;

      self.gpu_scene_flags |= GPU::SceneFlags::HasFilmGrain;
    });

  auto zero_fill_pass = vuk::make_pass(
    "zero fill",
    [](vuk::CommandBuffer& command_buffer, VUK_BA(vuk::eTransferWrite) dst) {
      command_buffer.fill_buffer(dst, 0_u32);
      return dst;
    }
  );

  update_buffer_if_dirty<GPU::Transforms>(self, vk_context, info.gpu_transforms, info.dirty_transform_ids);
  update_buffer_if_dirty<GPU::Material>(self, vk_context, info.gpu_materials, info.dirty_material_indices);

  // TODO: Keep track of updated lights and only update them.
  if (!point_lights.empty()) {
    auto point_lights_size_bytes = ox::size_bytes(point_lights);
    auto src_point_lights_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eCPUtoGPU,
      point_lights_size_bytes
    );
    std::memcpy(src_point_lights_buffer->mapped_ptr, point_lights.data(), point_lights_size_bytes);

    auto dst_point_lights_buffer = vuk::acquire_buf(
      "point lights",
      self.point_lights_buffer->subrange(0, point_lights_size_bytes),
      vuk::eMemoryRead
    );
    self.prepared_frame.point_lights_buffer = vuk::copy(
      std::move(src_point_lights_buffer),
      std::move(dst_point_lights_buffer)
    );
  } else {
    self.prepared_frame
      .point_lights_buffer = vuk::acquire_buf("point lights", *self.point_lights_buffer, vuk::eMemoryRead);
  }

  if (!spot_lights.empty()) {
    auto spot_lights_size_bytes = ox::size_bytes(spot_lights);
    auto src_spot_lights_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eCPUtoGPU,
      spot_lights_size_bytes
    );
    std::memcpy(src_spot_lights_buffer->mapped_ptr, spot_lights.data(), spot_lights_size_bytes);

    auto dst_spot_lights_buffer = vuk::acquire_buf(
      "spot lights",
      self.spot_lights_buffer->subrange(0, spot_lights_size_bytes),
      vuk::eMemoryRead
    );
    self.prepared_frame.spot_lights_buffer = vuk::copy(
      std::move(src_spot_lights_buffer),
      std::move(dst_spot_lights_buffer)
    );
  } else {
    self.prepared_frame
      .spot_lights_buffer = vuk::acquire_buf("spot lights", *self.spot_lights_buffer, vuk::eMemoryRead);
  }

  auto lights_info = GPU::Lights{
    .point_light_count = static_cast<u32>(point_lights.size()),
    .spot_light_count = static_cast<u32>(spot_lights.size()),
    .point_lights = self.point_lights_buffer->device_address,
    .spot_lights = self.spot_lights_buffer->device_address,
  };

  if (self.gpu_scene_flags & GPU::SceneFlags::HasAtmosphere) {
    auto atmosphere_buffer = self.renderer.vk_context->scratch_buffer(self.atmosphere);
    lights_info.atmosphere = atmosphere_buffer->device_address;
    self.prepared_frame.atmosphere_buffer = std::move(atmosphere_buffer);
  }

  if (self.gpu_scene_flags & GPU::SceneFlags::HasDirectionalLight) {
    auto directional_light_cascade_count = ox::max(1_u32, self.directional_light.cascade_count);

    auto directional_light_buffer = vk_context.scratch_buffer(self.directional_light);
    auto directional_light_cascades_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUtoCPU,
      directional_light_cascade_count * sizeof(GPU::DirectionalLightCascade)
    );

    lights_info.direction_light = directional_light_buffer->device_address;
    lights_info.direction_light_cascades = directional_light_cascades_buffer->device_address;
    if (self.directional_light.cascade_count > 0) {
      std::memcpy(
        directional_light_cascades_buffer->mapped_ptr,
        self.directional_light_cascades.data(),
        ox::size_bytes(self.directional_light_cascades)
      );
    }

    self.prepared_frame.directional_light_buffer = std::move(directional_light_buffer);
    self.prepared_frame.directional_light_cascades_buffer = std::move(directional_light_cascades_buffer);
  }

  self.prepared_frame.lights_buffer = vk_context.scratch_buffer(lights_info);

  self.render_queue_2d.init();

  if (!info.gpu_meshes.empty()) {
    self.meshes_buffer = vk_context.resize_buffer(
      std::move(self.meshes_buffer),
      vuk::MemoryUsage::eGPUonly,
      info.gpu_meshes.size_bytes()
    );
    self.prepared_frame.meshes_buffer = vk_context.upload_staging(info.gpu_meshes, *self.meshes_buffer);
  } else if (self.meshes_buffer) {
    self.prepared_frame.meshes_buffer = vuk::acquire_buf("meshes", *self.meshes_buffer, vuk::Access::eMemoryRead);
  }

  if (!info.gpu_mesh_instances.empty()) {
    self.mesh_instances_buffer = vk_context.resize_buffer(
      std::move(self.mesh_instances_buffer),
      vuk::MemoryUsage::eGPUonly,
      info.gpu_mesh_instances.size_bytes()
    );
    self.prepared_frame.mesh_instances_buffer = vk_context.upload_staging(
      info.gpu_mesh_instances,
      *self.mesh_instances_buffer
    );

    auto meshlet_instance_visibility_mask_size_bytes = (info.max_meshlet_instance_count + 31) / 32 * sizeof(u32);

    self.meshlet_instance_visibility_mask_buffer = vk_context.resize_buffer(
      std::move(self.meshlet_instance_visibility_mask_buffer),
      vuk::MemoryUsage::eGPUonly,
      meshlet_instance_visibility_mask_size_bytes
    );
    auto meshlet_instance_visibility_mask_buffer = vuk::acquire_buf(
      "meshlet instances visibility mask",
      *self.meshlet_instance_visibility_mask_buffer,
      vuk::eNone
    );
    self.prepared_frame.meshlet_instance_visibility_mask_buffer = zero_fill_pass(
      std::move(meshlet_instance_visibility_mask_buffer)
    );
  } else if (self.mesh_instances_buffer) {
    self.prepared_frame.mesh_instances_buffer = vuk::acquire_buf(
      "mesh instances",
      *self.mesh_instances_buffer,
      vuk::Access::eMemoryRead
    );
    self.prepared_frame.meshlet_instance_visibility_mask_buffer = vuk::acquire_buf(
      "meshlet instances visibility mask",
      *self.meshlet_instance_visibility_mask_buffer,
      vuk::eMemoryRead
    );
  }

  self.prepared_frame.mesh_instance_count = info.mesh_instance_count;
  self.prepared_frame.max_meshlet_instance_count = info.max_meshlet_instance_count;
  if (info.max_meshlet_instance_count > 0) {
    self.prepared_frame.meshlet_instances_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly,
      self.prepared_frame.max_meshlet_instance_count * sizeof(GPU::MeshletInstance)
    );
    self.prepared_frame.visible_meshlet_instances_indices_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly,
      self.prepared_frame.max_meshlet_instance_count * sizeof(u32)
    );
    self.prepared_frame.reordered_indices_buffer = vk_context.alloc_transient_buffer(
      vuk::MemoryUsage::eGPUonly,
      self.prepared_frame.max_meshlet_instance_count * Model::MAX_MESHLET_PRIMITIVES * 3 * sizeof(u32)
    );
  }

  auto debug_renderer_enabled = (bool)RendererCVar::cvar_enable_debug_renderer.get();

  if (debug_renderer_enabled) {
    const auto& lines = DebugRenderer::get_instance()->get_lines(false);
    auto [line_vertices, line_index_count] = DebugRenderer::get_vertices_from_lines(lines);

    const auto& triangles = DebugRenderer::get_instance()->get_triangles(false);
    auto [triangle_vertices, triangle_index_count] = DebugRenderer::get_vertices_from_triangles(triangles);

    const u32 index_count = line_index_count + triangle_index_count;
    OX_CHECK_LT(index_count, DebugRenderer::MAX_LINE_INDICES, "Increase DebugRenderer::MAX_LINE_INDICES");

    self.prepared_frame.line_index_count = line_index_count;
    self.prepared_frame.triangle_index_count = triangle_index_count;

    std::vector<DebugRenderer::Vertex> vertices = line_vertices;
    vertices.insert(vertices.end(), triangle_vertices.begin(), triangle_vertices.end());
    std::span<DebugRenderer::Vertex> vertices_span = line_vertices;

    if (!vertices.empty()) {
      self.debug_renderer_verticies_buffer = vk_context.resize_buffer(
        std::move(self.debug_renderer_verticies_buffer),
        vuk::MemoryUsage::eGPUonly,
        vertices_span.size_bytes()
      );
      self.prepared_frame.debug_renderer_verticies_buffer = vk_context.upload_staging(
        vertices_span,
        *self.debug_renderer_verticies_buffer
      );
    } else if (self.debug_renderer_verticies_buffer) {
      self.prepared_frame.debug_renderer_verticies_buffer = vuk::acquire_buf(
        "debug_renderer_verticies_buffer",
        *self.debug_renderer_verticies_buffer,
        vuk::Access::eMemoryRead
      );
    }

    DebugRenderer::reset();
  }

  auto gtao_enabled = (bool)RendererCVar::cvar_vbgtao_enable.get();
  if (gtao_enabled && self.viewport_size.x > 0) {
    self.vbgtao_info.thickness = RendererCVar::cvar_vbgtao_thickness.get();
    self.vbgtao_info.effect_radius = RendererCVar::cvar_vbgtao_radius.get();

    switch (RendererCVar::cvar_vbgtao_quality_level.get()) {
      case 0: {
        self.vbgtao_info.slice_count = 1;
        self.vbgtao_info.samples_per_slice_side = 2;
        break;
      }
      case 1: {
        self.vbgtao_info.slice_count = 2;
        self.vbgtao_info.samples_per_slice_side = 2;
        break;
      }
      case 2: {
        self.vbgtao_info.slice_count = 3;
        self.vbgtao_info.samples_per_slice_side = 3;
        break;
      }
      case 3: {
        self.vbgtao_info.slice_count = 9;
        self.vbgtao_info.samples_per_slice_side = 3;
        break;
      }
    }

    // vbgtao_info.noise_index = (RendererCVar::cvar_gtao_denoise_passes.get() > 0) ? (frameCounter % 64) : (0); //
    // TODO: If we have TAA
    self.vbgtao_info.noise_index = 0;
    self.vbgtao_info.final_power = RendererCVar::cvar_vbgtao_final_power.get();
  }

  if (!self.exposure_buffer) {
    self.exposure_buffer = vk_context.allocate_buffer_super(
      vuk::MemoryUsage::eGPUonly,
      sizeof(GPU::HistogramLuminance)
    );
  }

  self.prepared_frame.exposure_buffer = vuk::acquire_buf("exposure buffer", *self.exposure_buffer, vuk::eMemoryRead);
}
} // namespace ox
