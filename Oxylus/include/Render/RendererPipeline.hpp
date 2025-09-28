#pragma once

#include "Render/Renderer.hpp"
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
  vuk::Value<vuk::Buffer>& transforms_buffer
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
  vuk::Value<vuk::Buffer>& reordered_indices_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer
) -> vuk::Value<vuk::Buffer>;
auto draw_shadowmap(
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
auto generate_hiz(vuk::Value<vuk::ImageAttachment>& hiz_attachment, vuk::Value<vuk::ImageAttachment>& depth_attachment)
  -> void;

auto vis_decode(
  vuk::PersistentDescriptorSet& descriptor_set,
  vuk::Value<vuk::Buffer>& camera_buffer,
  vuk::Value<vuk::Buffer>& meshlet_instances_buffer,
  vuk::Value<vuk::Buffer>& mesh_instances_buffer,
  vuk::Value<vuk::Buffer>& meshes_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer,
  vuk::Value<vuk::Buffer>& materials_buffer,
  vuk::Value<vuk::ImageAttachment>& visbuffer_attachment,
  vuk::Value<vuk::ImageAttachment>& albedo_attachment,
  vuk::Value<vuk::ImageAttachment>& normal_attachment,
  vuk::Value<vuk::ImageAttachment>& emissive_attachment,
  vuk::Value<vuk::ImageAttachment>& metallic_roughness_occlusion_attachment
) -> void;

//
// Forward 2D Pipeline
//
auto forward_2d_pass(
  Renderer::RenderQueue2D& rq2d,
  vuk::PersistentDescriptorSet& descriptor_set,
  vuk::Value<vuk::ImageAttachment>& final_attachment,
  vuk::Value<vuk::ImageAttachment>& depth_attachment,
  vuk::Value<vuk::Buffer>& vertex_buffer_2d,
  vuk::Value<vuk::Buffer>& materials_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer,
  vuk::Value<vuk::Buffer>& transforms_buffer
) -> void;

//
// Atmosphere Pipeline
//
auto atmosphere_pass(
  vuk::Value<vuk::Buffer>& atmosphere_buffer,
  vuk::Value<vuk::Buffer>& lights_buffer,
  vuk::Value<vuk::Buffer>& camera_buffer,
  vuk::Value<vuk::ImageAttachment>& sky_transmittance_lut_attachment,
  vuk::Value<vuk::ImageAttachment>& sky_multiscatter_lut_attachment,
  vuk::Value<vuk::ImageAttachment>& sky_view_lut_attachment,
  vuk::Value<vuk::ImageAttachment>& sky_aerial_perspective_attachment,
  vuk::Value<vuk::ImageAttachment>& final_attachment,
  vuk::Value<vuk::ImageAttachment>& depth_attachment
) -> void;
} // namespace ox
