#pragma once

#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
//
// Geometry Prepass Pipeline
//
auto cull_meshes(
  GPU::CullFlags cull_flags,
  u32 mesh_instance_count,
  VkContext& ctx,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> vuk::Value<vuk::Buffer>;
auto cull_meshlets(
  bool late,
  GPU::CullFlags cull_flags,
  VkContext& ctx,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::Buffer>& cull_meshlets_cmd_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_indices_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instance_visibility_mask_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> vuk::Value<vuk::Buffer>;
auto clear_visbuffer(
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment, vuk::Value<vuk::ImageAttachment>& overdraw_attachment
) -> void;
auto draw_visbuffer(
  bool late,
  vuk::PersistentDescriptorSet& descriptor_set,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment,
  vuk::Value<vuk::ImageAttachment>& overdraw_attachment,
  vuk::Value<vuk::Buffer>& draw_command_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& materials_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> void;
auto draw_visbuffer_depth_only(
  bool late,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::Buffer>& draw_command_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer
) -> void;
auto generate_hiz(vuk::Value<vuk::ImageAttachment>& hiz_attachment, vuk::Value<vuk::ImageAttachment>& depth_attachment)
  -> void;
} // namespace ox
