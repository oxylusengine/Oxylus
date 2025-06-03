#pragma once

#include <vuk/Types.hpp>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/Allocator.hpp>
#include <vuk/runtime/vk/Descriptor.hpp>

#include "Asset/Texture.hpp"
#include "RenderPipeline.hpp"
#include "Scene/ECSModule/Core.hpp"
#include "Scene/SceneGPU.hpp"

namespace ox {
class EasyRenderPipeline : public RenderPipeline {
public:
  EasyRenderPipeline() = default;
  ~EasyRenderPipeline() override = default;

  auto init(VkContext& vk_context) -> void override;
  auto deinit() -> void override;

  auto on_render(VkContext& vk_context, const RenderInfo& render_info) -> vuk::Value<vuk::ImageAttachment> override;

  auto on_update(Scene* scene) -> void override;

private:
  enum BindlessID : u32 {
    Samplers = 0,
    SampledImages = 1,
  };

  struct DrawBatch2D {
    vuk::Name pipeline_name = {};
    u32 offset = 0;
    u32 count = 0;
  };

  enum RenderFlags2D : u32 {
    RENDER_FLAGS_2D_NONE = 0,

    RENDER_FLAGS_2D_SORT_Y = 1 << 0,
    RENDER_FLAGS_2D_FLIP_X = 1 << 1,
  };

  struct SpriteGPUData {
    alignas(4) u32 material_id16_ypos16 = 0;
    alignas(4) u32 flags16_distance16 = 0;
    alignas(4) u32 transform_id = 0;

    bool operator>(const SpriteGPUData& other) const {
      union SortKey {
        struct {
          // The order of members is important here, it means the sort priority (low to high)!
          u64 distance_y : 32;
          u64 distance_z : 32;
        } bits;

        u64 value;
      };
      static_assert(sizeof(SortKey) == sizeof(u64));
      const SortKey a = {
          .bits =
              {
                  .distance_y = math::unpack_u32_low(flags16_distance16) & RENDER_FLAGS_2D_SORT_Y
                                    ? math::unpack_u32_high(material_id16_ypos16)
                                    : 0u,
                  .distance_z = math::unpack_u32_high(flags16_distance16),
              },
      };
      const SortKey b = {
          .bits =
              {
                  .distance_y = math::unpack_u32_low(other.flags16_distance16) & RENDER_FLAGS_2D_SORT_Y
                                    ? math::unpack_u32_high(other.material_id16_ypos16)
                                    : 0u,
                  .distance_z = math::unpack_u32_high(other.flags16_distance16),
              },
      };
      return a.value > b.value;
    }
  };

  struct RenderQueue2D {
    std::vector<DrawBatch2D> batches = {};
    std::vector<SpriteGPUData> sprite_data = {};

    vuk::Name current_pipeline_name = {};

    u32 num_sprites = 0;
    u32 previous_offset = 0;

    u32 last_batches_size = 0;
    u32 last_sprite_data_size = 0;

    void init() {
      clear();
      batches.reserve(last_batches_size);
      sprite_data.reserve(last_sprite_data_size);
    }

    // TODO: this will take a list of materials
    // TODO: sort pipelines
    void update() {
      const vuk::Name
          pipeline_name = "2d_forward_pipeline"; // TODO: hardcoded until we have a modular material shader system
      if (current_pipeline_name != pipeline_name) {
        batches.emplace_back(DrawBatch2D{
            .pipeline_name = pipeline_name, .offset = previous_offset, .count = num_sprites - previous_offset});
        current_pipeline_name = pipeline_name;
      }

      previous_offset = num_sprites;
    }

    void add(const SpriteComponent& sprite,
             const float& position_y,
             u32 transform_id,
             u32 material_id,
             const float distance) {
      u16 flags = 0;
      if (sprite.sort_y)
        flags |= RENDER_FLAGS_2D_SORT_Y;

      if (sprite.flip_x)
        flags |= RENDER_FLAGS_2D_FLIP_X;

      const u32 flags_and_distance = math::pack_u16(flags, glm::packHalf1x16(distance));
      const u32 materialid_and_ypos = math::pack_u16(static_cast<u16>(material_id), glm::packHalf1x16(position_y));

      sprite_data.emplace_back(SpriteGPUData{
          .material_id16_ypos16 = materialid_and_ypos,
          .flags16_distance16 = flags_and_distance,
          .transform_id = transform_id,
      });

      num_sprites += 1;
    }

    void sort() { std::ranges::sort(sprite_data, std::greater<SpriteGPUData>()); }

    void clear() {
      num_sprites = 0;
      previous_offset = 0;
      last_batches_size = static_cast<u32>(batches.size());
      last_sprite_data_size = static_cast<u32>(sprite_data.size());
      current_pipeline_name = {};

      batches.clear();
      sprite_data.clear();
    }
  };

  vuk::Unique<vuk::PersistentDescriptorSet> descriptor_set_01 = vuk::Unique<vuk::PersistentDescriptorSet>();

  RenderQueue2D render_queue_2d = {};
  bool saved_camera = false;

  std::span<GPU::Transforms> transforms = {};
  std::vector<GPU::TransformID> dirty_transforms = {};
  vuk::Unique<vuk::Buffer> transforms_buffer = vuk::Unique<vuk::Buffer>();

  std::vector<GPU::Material> gpu_materials = {};
  std::vector<vuk::ImageView> texture_views = {};
  GPU::CameraData camera_data = {};

  option<GPU::Atmosphere> atmosphere = nullopt;
  option<GPU::Sun> sun = nullopt;

  option<GPU::HistogramInfo> histogram_info = nullopt;

  Texture sky_transmittance_lut_view;
  Texture sky_multiscatter_lut_view;

  auto sky_pass(VkContext& vk_context,
                vuk::Value<vuk::Buffer>& camera_buffer,
                vuk::Value<vuk::ImageAttachment>& final_attachment,
                vuk::Value<vuk::ImageAttachment>& depth_attachment) -> void;
};
} // namespace ox
