#pragma once

#include "Render/Vulkan/VkContext.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
//
// Geometry Prepass Pipeline
//
auto cull_meshes(
  VkContext& ctx,
  GPU::CullFlags cull_flags,
  u32 mesh_instance_count,
  glm::mat4& frustum_projection_view,
  glm::vec4& observer_position,
  glm::vec2& observer_resolution,
  f32 acceptable_lod_error,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment
) -> vuk::Value<vuk::Buffer>;
auto cull_meshlets(
  VkContext& ctx,
  bool late,
  GPU::CullFlags cull_flags,
  f32 near_clip,
  glm::vec2& resolution,
  glm::mat4& projection_view,
  vuk::Value<vuk::ImageAttachment>& hiz_attachment,
  vuk::Value<vuk::Buffer>& cull_meshlets_cmd_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& early_visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& late_visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_indices_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instance_visibility_mask_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer
) -> vuk::Value<vuk::Buffer>;
auto clear_visbuffer(
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment, vuk::Value<vuk::ImageAttachment>& overdraw_attachment
) -> void;
auto draw_visbuffer(
  bool late,
  glm::mat4& projection_view,
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
  vuk::Value<vuk::Buffer>& materials_buffer
) -> void;
auto cull_shadowmap_meshlets(
  VkContext& ctx,
  u32 cascade_index,
  glm::vec2& resolution,
  glm::mat4& projection_view,
  vuk::Value<vuk::Buffer>& cull_meshlets_cmd_buffer,
  vuk::Value<vuk::Buffer>& all_visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_count_buffer,
  vuk::Value<vuk::Buffer>& visible_meshlet_instances_indices_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instance_visibility_mask_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer
) -> vuk::Value<vuk::Buffer>;
auto draw_shadowmap(
  bool late,
  u32 cascade_index,
  glm::mat4& projection_view,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::Buffer>& draw_command_buffer,
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer
) -> void;
auto generate_hiz(
  vuk::Value<vuk::ImageAttachment>& hiz_attachment, vuk::Value<vuk::ImageAttachment>& depth_attachment, bool is_array
) -> void;
} // namespace ox
