#include "AnimationEditorPanel.hpp"

#include <algorithm>
#include <ankerl/svector.h>
#include <array>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <icons/IconsMaterialDesignIcons.h>
#include <imgui.h>
#include <string>
#include <vuk/vsl/Core.hpp>

#include "Animation/AnimationClip.hpp"
#include "Animation/Skeleton.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/DebugRenderer.hpp"
#include "Scene/Components.hpp"
#include "UI/UI.hpp"
#include "Utils/AnimationAssets.hpp"
#include "Utils/EditorGrid.hpp"
#include "Utils/Log.hpp"

namespace ox {
// evenly spaced around the model and well above its horizon, so nothing is lit from underneath
constexpr static std::array<f32, STUDIO_LIGHT_COUNT> STUDIO_LIGHT_AZIMUTHS = {0.52f, 2.62f, 4.71f};
constexpr static f32 STUDIO_LIGHT_ELEVATION = 0.95f;
// key, fill, rim
constexpr static std::array<f32, STUDIO_LIGHT_COUNT> STUDIO_LIGHT_GAINS = {6.0f, 2.4f, 3.4f};
constexpr static glm::vec4 SKELETON_BONE_COLOR = {0.20f, 0.95f, 1.00f, 1.00f};
// blender's proportions: the ring sits a tenth of the way down the bone and is a tenth as wide
constexpr static f32 BONE_RING_POSITION = 0.1f;
constexpr static f32 BONE_RING_RADIUS = 0.1f;
constexpr static glm::vec4 SKELETON_AXIS_X_COLOR = {0.95f, 0.25f, 0.25f, 1.00f};
constexpr static glm::vec4 SKELETON_AXIS_Y_COLOR = {0.35f, 0.90f, 0.35f, 1.00f};
constexpr static glm::vec4 SKELETON_AXIS_Z_COLOR = {0.35f, 0.55f, 0.95f, 1.00f};

constexpr static std::array<glm::vec3, STUDIO_LIGHT_COUNT> STUDIO_LIGHT_COLORS = {
  glm::vec3(1.00f, 0.96f, 0.90f),
  glm::vec3(0.88f, 0.93f, 1.00f),
  glm::vec3(1.00f, 1.00f, 1.00f),
};

// blender's octahedral bone: a square ring near the head, fanned out to the head and in to the tail
auto draw_bone_octahedron(
  DebugRenderer& debug_renderer,
  const glm::vec3& head,
  const glm::vec3& tail,
  const glm::vec3& roll_reference,
  const glm::vec4& color
) -> void {
  const auto axis = tail - head;
  const auto length = glm::length(axis);
  if (length <= 1e-6f) {
    return;
  }

  const auto direction = axis / length;

  // the ring twists with the joint, which is what makes a bad roll visible at a glance. the
  // reference arrives scaled by the world transform, so the parallel test needs it normalized
  const auto reference_length = glm::length(roll_reference);
  auto reference = reference_length > 1e-6f ? roll_reference / reference_length : glm::vec3(0.0f, 1.0f, 0.0f);
  if (glm::abs(glm::dot(reference, direction)) > 0.99f) {
    reference = glm::abs(direction.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
  }

  const auto right = glm::normalize(glm::cross(direction, reference));
  const auto up = glm::cross(right, direction);

  const auto ring_center = head + direction * (length * BONE_RING_POSITION);
  const auto radius = length * BONE_RING_RADIUS;
  const glm::vec3 ring[4] = {
    ring_center + right * radius,
    ring_center + up * radius,
    ring_center - right * radius,
    ring_center - up * radius,
  };

  for (auto corner = 0_u32; corner < 4; ++corner) {
    const auto& next = ring[(corner + 1) % 4];
    debug_renderer.draw_line(head, ring[corner], 1.0f, color, false);
    debug_renderer.draw_line(ring[corner], next, 1.0f, color, false);
    debug_renderer.draw_line(ring[corner], tail, 1.0f, color, false);
  }
}

AnimationEditorPanel::AnimationEditorPanel() : EditorPanelState("Animation Editor", ICON_MDI_ANIMATION_PLAY, false) {
  this->window_default_size = {900, 640};
}

AnimationEditorPanel::~AnimationEditorPanel() {
  // the scene releases the refs its components hold as their entities go away
  preview_animator = {};
  preview_sky = {};
  preview_scene.reset();
}

auto AnimationEditorPanel::open_asset(this AnimationEditorPanel& self, const UUID& uuid) -> void {
  ZoneScoped;

  if (!uuid) {
    return;
  }

  auto& asset_man = App::mod<AssetManager>();

  auto asset_type = AssetType::None;
  if (auto asset = asset_man.get_asset(uuid)) {
    asset_type = asset->type;
  }

  auto resolved_model = UUID(nullptr);
  auto resolved_clip = UUID(nullptr);
  switch (asset_type) {
    case AssetType::Animation: {
      resolved_clip = uuid;
      resolved_model = find_source_model(uuid);
      break;
    }
    case AssetType::Model: {
      resolved_model = uuid;
      const auto clips = model_animation_clips(uuid);
      resolved_clip = clips.empty() ? UUID(nullptr) : clips.front();
      break;
    }
    default: return;
  }

  if (!resolved_model) {
    OX_LOG_ERROR("Cannot preview '{}': its source model is not in the asset registry.", uuid.str());
    return;
  }

  self.model_uuid = resolved_model;
  self.clip_uuid = resolved_clip;
  self.visible = true;

  self.sync_preview_model();
}

auto AnimationEditorPanel::ensure_preview_scene(this AnimationEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  if (self.preview_scene) {
    return;
  }

  self.preview_scene = std::make_unique<Scene>("AnimationPreviewScene");
  auto& scene = *self.preview_scene;

  // ambient and background only: the rig below does the lighting, and only one directional light
  // is ever kept, so three of them would collapse into whichever was gathered last
  self.preview_sky = scene.create_entity("preview_environment", true);
  self.preview_sky.set<TransformComponent>({}).set<SkyComponent>({
    .solid_color = self.preview_background,
    .ambient_color = glm::vec3(0.06f),
    .texture = UUID(nullptr),
  });

  for (auto i = 0_u32; i < STUDIO_LIGHT_COUNT; ++i) {
    self.preview_lights[i] = scene.create_entity(std::string(stack.format("preview_light_{}", i)), true);
    self.preview_lights[i].set<TransformComponent>({}).set<LightComponent>({
      .type = LightComponent::LightType::Point,
      .color = STUDIO_LIGHT_COLORS[i],
      .cast_shadows = false,
    });
  }

  scene.create_entity("preview_camera", true)
    .set<CameraComponent>({.fov = 60.0f, .far_clip = 10000.0f, .near_clip = 0.05f})
    .set<TransformComponent>({});

  scene.renderer_cvar.cvar_draw_bounding_boxes.set(false);
  scene.renderer_cvar.cvar_bloom_enable.set(false);
  scene.renderer_cvar.cvar_vbgtao_enable.set(false);
  scene.renderer_cvar.cvar_contact_shadows_enabled.set(false);
  scene.renderer_cvar.cvar_ddgi_enable.set(false);

  // clips only advance while the scene runs, and this preview exists to show them running
  scene.runtime_start();
}

auto AnimationEditorPanel::sync_preview_model(this AnimationEditorPanel& self) -> void {
  ZoneScoped;

  if (self.preview_model_uuid == self.model_uuid) {
    self.set_clip(self.clip_uuid);
    return;
  }

  // rebuilt rather than reparented: tearing the scene down is what releases the previous model's
  // references, and a preview holds nothing else worth keeping
  self.preview_animator = {};
  self.preview_sky = {};
  self.preview_scene.reset();
  self.preview_model_uuid = {};

  if (!self.model_uuid) {
    return;
  }

  self.ensure_preview_scene();

  self.preview_animator = self.preview_scene->create_model_entity(self.model_uuid);
  if (!self.preview_animator) {
    OX_LOG_ERROR("Failed to spawn '{}' into the animation preview.", self.model_uuid.str());
    return;
  }

  self.preview_model_uuid = self.model_uuid;
  self.frame_model();
  self.set_clip(self.clip_uuid);
}

auto AnimationEditorPanel::set_clip(this AnimationEditorPanel& self, const UUID& uuid) -> void {
  ZoneScoped;

  self.clip_uuid = uuid;
  if (!self.preview_animator || !self.preview_animator.has<AnimatorComponent>()) {
    return;
  }

  auto& animator = self.preview_animator.get_mut<AnimatorComponent>();
  if (animator.clip_uuid == uuid) {
    return;
  }

  // whoever writes the uuid owns the ref, so the swap releases the old one and takes the new
  auto& asset_man = App::mod<AssetManager>();
  if (uuid && !asset_man.load_asset(uuid)) {
    return;
  }

  if (animator.clip_uuid) {
    asset_man.unload_asset(animator.clip_uuid);
  }

  animator.clip_uuid = uuid;
  self.preview_animator.modified<AnimatorComponent>();
}

auto AnimationEditorPanel::frame_model(this AnimationEditorPanel& self) -> void {
  ZoneScoped;

  auto bounds_min = glm::vec3(std::numeric_limits<f32>::max());
  auto bounds_max = glm::vec3(std::numeric_limits<f32>::lowest());

  if (auto model = App::mod<AssetManager>().get_model(self.model_uuid)) {
    for (const auto& mesh : model->gpu_meshes) {
      const auto half_extent = mesh.bounds.aabb_extent * 0.5f;
      bounds_min = glm::min(bounds_min, mesh.bounds.aabb_center - half_extent);
      bounds_max = glm::max(bounds_max, mesh.bounds.aabb_center + half_extent);
    }
  }

  if (bounds_min.x > bounds_max.x) {
    self.preview_target = glm::vec3(0.0f);
    self.preview_radius = 1.0f;
    self.preview_distance = 6.0f;
    self.preview_grid_distance = 100.0f;
    self.update_studio_lights();
    return;
  }

  self.preview_radius = glm::max(glm::length(bounds_max - bounds_min) * 0.5f, 0.01f);
  self.preview_target = (bounds_min + bounds_max) * 0.5f;
  self.preview_distance = self.preview_radius * 2.5f;
  // the grid is drawn in world units, so it has to grow with the rig or it vanishes under it
  self.preview_grid_distance = self.preview_radius * 20.0f;

  self.update_studio_lights();
}

auto AnimationEditorPanel::update_studio_lights(this AnimationEditorPanel& self) -> void {
  ZoneScoped;

  const auto distance = self.preview_radius * 2.6f;

  for (auto i = 0_u32; i < STUDIO_LIGHT_COUNT; ++i) {
    auto light = self.preview_lights[i];
    if (!light) {
      continue;
    }

    const auto azimuth = STUDIO_LIGHT_AZIMUTHS[i];
    const auto offset = glm::vec3(
      std::cos(STUDIO_LIGHT_ELEVATION) * std::sin(azimuth),
      std::sin(STUDIO_LIGHT_ELEVATION),
      std::cos(STUDIO_LIGHT_ELEVATION) * std::cos(azimuth)
    );

    light.get_mut<TransformComponent>().position = self.preview_target + offset * distance;

    auto& lc = light.get_mut<LightComponent>();
    // the falloff is inverse square, so the gain has to carry the distance or a big rig goes black
    lc.intensity = STUDIO_LIGHT_GAINS[i] * self.preview_light_intensity * distance * distance;
    // generous enough that the windowed cutoff never bites into the model itself
    lc.radius = distance * 4.0f;

    light.modified<TransformComponent>();
    light.modified<LightComponent>();
  }
}

auto AnimationEditorPanel::draw_skeleton(this AnimationEditorPanel& self) -> void {
  ZoneScoped;

  if (!self.preview_animator) {
    return;
  }

  const auto instance_it = self.preview_scene->entity_to_animation_instance_map.find(self.preview_animator);
  if (instance_it == self.preview_scene->entity_to_animation_instance_map.end()) {
    return;
  }

  const auto* instance = self.preview_scene->animation_instances.slot(instance_it->second);
  if (instance == nullptr || instance->pose.model_space_transforms.empty()) {
    return;
  }

  auto skeleton = App::mod<AssetManager>().get_skeleton(instance->skeleton_uuid);
  if (!skeleton) {
    return;
  }

  // the pose is in skin space, which glTF pins to the animator root rather than to the mesh node
  const auto to_world = Scene::get_world_transform(self.preview_animator);
  const auto bone_position = [&](const usize bone) {
    return glm::vec3(to_world * glm::vec4(instance->pose.model_space_transforms[bone].translation(), 1.0f));
  };

  const auto bone_count = ox::min(instance->pose.model_space_transforms.size(), skeleton->parent_indices.size());
  const auto bone_basis = [&](const usize bone) {
    return glm::mat3(to_world) * glm::mat3_cast(instance->pose.model_space_transforms[bone].rotation);
  };

  // a glTF joint carries no tail, so a bone body is the segment down to each child. a joint with no
  // children has no body and would otherwise not be drawn at all
  auto has_child = ankerl::svector<bool, 64>(bone_count, false);
  for (auto bone = 0_sz; bone < bone_count; ++bone) {
    const auto parent = skeleton->parent_indices[bone];
    if (parent >= 0 && static_cast<usize>(parent) < bone_count) {
      has_child[static_cast<usize>(parent)] = true;
    }
  }

  auto& debug_renderer = App::mod<DebugRenderer>();
  for (auto bone = 0_sz; bone < bone_count; ++bone) {
    const auto position = bone_position(bone);

    const auto parent = skeleton->parent_indices[bone];
    if (parent >= 0) {
      const auto head = static_cast<usize>(parent);
      draw_bone_octahedron(debug_renderer, bone_position(head), position, bone_basis(head)[1], SKELETON_BONE_COLOR);
    }

    if (has_child[bone]) {
      continue;
    }

    // proportional to the rig, so a tip stays visible without swallowing a small one
    const auto axis_length = self.preview_radius * 0.02f;
    const auto basis = bone_basis(bone);
    debug_renderer.draw_line(position, position + basis[0] * axis_length, 1.0f, SKELETON_AXIS_X_COLOR, false);
    debug_renderer.draw_line(position, position + basis[1] * axis_length, 1.0f, SKELETON_AXIS_Y_COLOR, false);
    debug_renderer.draw_line(position, position + basis[2] * axis_length, 1.0f, SKELETON_AXIS_Z_COLOR, false);
  }
}

auto AnimationEditorPanel::on_update(this AnimationEditorPanel& self) -> void {
  ZoneScoped;

  // the preview scene ticks in `draw_preview`, immediately before its render
}

auto AnimationEditorPanel::draw_preview(
  this AnimationEditorPanel& self, const vuk::ImageAttachment& swapchain_attachment
) -> void {
  ZoneScoped;

  const auto available = ImGui::GetContentRegionAvail();
  self.preview_size = {glm::max(available.x, 32.0f), glm::max(available.y, 32.0f)};

  auto attachment_info = swapchain_attachment;
  attachment_info.extent = vuk::Extent3D{
    static_cast<u32>(self.preview_size.x),
    static_cast<u32>(self.preview_size.y),
    1u,
  };

  auto attachment = vuk::declare_ia("animation preview", attachment_info);
  attachment = vuk::clear_image(
    std::move(attachment),
    vuk::ClearColor(
      self.preview_background.r,
      self.preview_background.g,
      self.preview_background.b,
      self.preview_background.a
    )
  );

  const auto orbit = glm::vec3(
    std::cos(self.preview_orbit_pitch) * std::sin(self.preview_orbit_yaw),
    std::sin(self.preview_orbit_pitch),
    std::cos(self.preview_orbit_pitch) * std::cos(self.preview_orbit_yaw)
  );
  const auto camera_position = self.preview_target + orbit * self.preview_distance;

  self.preview_scene->world.query_builder<TransformComponent, CameraComponent>().build().each(
    [&](TransformComponent& tc, CameraComponent& cc) {
      tc.position = camera_position;
      tc.rotation = glm::quatLookAt(glm::normalize(self.preview_target - camera_position), glm::vec3(0.0f, 1.0f, 0.0f));
      cc.position = camera_position;
    }
  );

  if (self.preview_sky.has<SkyComponent>()) {
    self.preview_sky.get_mut<SkyComponent>().solid_color = self.preview_background;
  }

  // pause and speed ride on the component `update_animations` already reads, so the scene still
  // gets a full update every frame, which `RendererInstance::render` asserts on
  if (self.preview_animator && self.preview_animator.has<AnimatorComponent>()) {
    auto& animator = self.preview_animator.get_mut<AnimatorComponent>();
    animator.playing = self.preview_playing;
    animator.speed = self.preview_speed;
  }

  // paired with the render below the way ThumbnailManager pairs them: the panel can be opened from
  // inside another panel's on_render, by which point update_all has already run past it
  self.preview_scene->runtime_update(App::get_timestep());

  // after the tick that rebuilt the pose and before the render, which is what drains the global
  // debug line list through this scene's own renderer update
  self.preview_scene->renderer_cvar.cvar_enable_debug_renderer.set(self.preview_skeleton_enabled);
  if (self.preview_skeleton_enabled) {
    self.draw_skeleton();
  }

  if (self.preview_grid_enabled) {
    if (auto* renderer_instance = self.preview_scene->get_renderer_instance(); renderer_instance != nullptr) {
      add_editor_grid_stage(*renderer_instance, self.preview_grid_distance);
    }
  }

  auto image = self.preview_scene->render(
    std::move(attachment),
    glm::ivec2(0),
    glm::ivec2(static_cast<i32>(self.preview_size.x), static_cast<i32>(self.preview_size.y)),
    glm::ivec2(static_cast<i32>(self.preview_size.x), static_cast<i32>(self.preview_size.y)),
    false
  );

  const auto image_size = ImVec2(self.preview_size.x, self.preview_size.y);
  const auto image_cursor = ImGui::GetCursorPos();
  UI::image(std::move(image), image_size);

  // an image is not an interactive item, so a drag over it would reach ImGui as a window move, and
  // the invisible button claims the mouse for the orbit instead
  ImGui::SetCursorPos(image_cursor);
  ImGui::InvisibleButton("##preview_input", image_size, ImGuiButtonFlags_MouseButtonLeft);

  if (ImGui::IsItemActive()) {
    const auto delta = ImGui::GetIO().MouseDelta;
    self.preview_orbit_yaw -= delta.x * 0.01f;
    self.preview_orbit_pitch = std::clamp(self.preview_orbit_pitch + delta.y * 0.01f, -1.5f, 1.5f);
  }

  if (ImGui::IsItemHovered()) {
    const auto step = glm::max(self.preview_distance * 0.1f, 0.05f);
    self.preview_distance = glm::max(self.preview_distance - ImGui::GetIO().MouseWheel * step, 0.05f);
  }
}

auto AnimationEditorPanel::draw_toolbar(this AnimationEditorPanel& self) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto& asset_man = App::mod<AssetManager>();

  if (UI::button(self.preview_playing ? ICON_MDI_PAUSE : ICON_MDI_PLAY)) {
    self.preview_playing = !self.preview_playing;
  }

  ImGui::SameLine();
  if (UI::toggle_button(ICON_MDI_GRID, self.preview_grid_enabled)) {
    self.preview_grid_enabled = !self.preview_grid_enabled;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Toggle grid");
  }

  ImGui::SameLine();
  if (UI::toggle_button(ICON_MDI_BONE, self.preview_skeleton_enabled)) {
    self.preview_skeleton_enabled = !self.preview_skeleton_enabled;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Toggle skeleton");
  }

  ImGui::SameLine();
  if (UI::button(ICON_MDI_IMAGE_FILTER_CENTER_FOCUS)) {
    self.frame_model();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("Frame model");
  }

  auto clips = model_animation_clips(self.model_uuid);
  if (clips.empty()) {
    clips = sibling_animation_clips(self.clip_uuid);
  }

  auto clip_names = ankerl::svector<const c8*, 8>();
  auto selected_clip = 0;
  for (const auto& clip_uuid : clips) {
    const c8* label = nullptr;
    if (auto clip = asset_man.get_animation(clip_uuid)) {
      label = stack.null_terminate_cstr(clip->name);
    }

    if (label == nullptr) {
      continue;
    }

    if (clip_uuid == self.clip_uuid) {
      selected_clip = static_cast<i32>(clip_names.size());
    }

    clip_names.emplace_back(label);
  }

  auto chosen = UUID(nullptr);
  if (!clip_names.empty()) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (
      ImGui::Combo("##clip", &selected_clip, clip_names.data(), static_cast<i32>(clip_names.size())) &&
      selected_clip >= 0 && selected_clip < static_cast<i32>(clips.size())
    ) {
      chosen = clips[static_cast<usize>(selected_clip)];
    }
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  ImGui::SliderFloat("Speed", &self.preview_speed, 0.0f, 4.0f);

  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::SliderFloat("Light", &self.preview_light_intensity, 0.0f, 4.0f)) {
    self.update_studio_lights();
  }

  ImGui::SameLine();
  ImGui::ColorEdit3(
    "Background",
    glm::value_ptr(self.preview_background),
    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha
  );

  if (auto clip = asset_man.get_animation(self.clip_uuid)) {
    ImGui::SameLine();
    ImGui::TextUnformatted(stack.format_char("{:.2f}s, {} frames", clip->duration, clip->frame_count));
  }

  // after every guard is gone, because load and unload take the registry write lock
  if (chosen) {
    self.set_clip(chosen);
  }
}

auto AnimationEditorPanel::on_render(this AnimationEditorPanel& self, const vuk::ImageAttachment swapchain_attachment)
  -> void {
  ZoneScoped;

  if (!self.on_begin()) {
    self.on_end();
    return;
  }

  if (!self.model_uuid || !self.preview_scene) {
    ImGui::TextUnformatted("Open an animation clip or a skinned model to preview it.");
    self.on_end();
    return;
  }

  self.draw_toolbar();
  self.draw_preview(swapchain_attachment);

  self.on_end();
}
} // namespace ox
