#pragma once

#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
struct MeshRenderContext {
  vuk::Value<vuk::Buffer> camera_buffer;
  vuk::Value<vuk::Buffer> transforms_buffer;
  vuk::Value<vuk::Buffer> materials_buffer;
  vuk::Value<vuk::Buffer> meshes_buffer;
  vuk::Value<vuk::Buffer> mesh_instances_buffer;
  vuk::Value<vuk::Buffer> meshlet_instances_buffer;
  vuk::Value<vuk::Buffer> meshlet_instance_visibility_mask_buffer;
  vuk::Value<vuk::Buffer> visible_meshlet_instances_indices_buffer;
  vuk::Value<vuk::Buffer> reordered_indices_buffer;
  vuk::Value<vuk::Buffer> visible_meshlet_instances_count_buffer;

  vuk::Value<vuk::ImageAttachment> depth_attachment;
  vuk::Value<vuk::ImageAttachment> hiz_attachment;

  u32 mesh_instance_count = 0;
  u32 max_meshlet_instance_count = 0;
};

class MeshRenderer {
public:
  MeshRenderer(VkContext& vk_context, vuk::PersistentDescriptorSet& bindless_set);

  auto set_extent(this MeshRenderer& self, vuk::Extent3D extent) -> MeshRenderer&;
  auto set_visbuffer_encode_pipeline(this MeshRenderer& self, const std::string& name) -> MeshRenderer&;
  auto set_pass_name(this MeshRenderer& self, const std::string& name) -> MeshRenderer&;
  auto set_cull_flags(this MeshRenderer& self, GPU::CullFlags flags) -> MeshRenderer&;
  auto disable_hiz(this MeshRenderer& self) -> MeshRenderer&;

  auto render(MeshRenderContext& ctx) -> std::tuple<vuk::Value<vuk::ImageAttachment>, vuk::Value<vuk::ImageAttachment>>;

  auto cull_meshes(MeshRenderContext& ctx) -> vuk::Value<vuk::Buffer>;

  auto cull_meshlets(MeshRenderContext& ctx, bool late, vuk::Value<vuk::Buffer>& cull_meshlets_cmd_buffer)
    -> vuk::Value<vuk::Buffer>;

  auto draw_visbuffer(
    MeshRenderContext& ctx,
    bool late,
    vuk::PersistentDescriptorSet& descriptor_set,
    vuk::Value<vuk::ImageAttachment>& visbuffer_attachment,
    vuk::Value<vuk::ImageAttachment>& overdraw_attachment,
    vuk::Value<vuk::Buffer>& draw_command_buffer
  ) -> void;

  auto generate_hiz(MeshRenderContext& ctx) -> void;

private:
  VkContext& vk_context_;
  vuk::PersistentDescriptorSet& bindless_set_;

  vuk::Extent3D extent_ = {};
  std::string pass_name_ = {};
  GPU::CullFlags cull_flags_ = GPU::CullFlags::MicroTriangles | GPU::CullFlags::TriangleBackFace;
  bool enable_hiz = true;
};

} // namespace ox
