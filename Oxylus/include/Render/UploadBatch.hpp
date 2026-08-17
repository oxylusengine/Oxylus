#pragma once

#include <shared_mutex>
#include <span>
#include <vector>
#include <vuk/Value.hpp>
#include <vuk/runtime/vk/Allocator.hpp>

#include "Core/Arc.hpp"
#include "Core/Types.hpp"

namespace ox {
class RenderContext;

struct UploadBatch : ManagedObj {
  struct DescriptorWrite {
    u32 binding = 0;
    u32 array_element = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
    VkDescriptorImageInfo image_info = {};
  };

  std::shared_mutex mutex = {};
  std::vector<vuk::UntypedValue> submitted = {};
  std::vector<vuk::Unique<vuk::Buffer>> staging = {};
  std::vector<DescriptorWrite> descriptor_writes = {};

  ~UploadBatch();

  static auto create() -> Arc<UploadBatch>;

  auto add_upload(this UploadBatch& self, std::span<vuk::UntypedValue> values) -> void;
  auto take_staging(this UploadBatch& self, std::span<vuk::Unique<vuk::Buffer>> buffers) -> void;
  auto add_descriptor_write(this UploadBatch& self, const DescriptorWrite& write) -> void;

  auto flush(this UploadBatch& self, RenderContext& render_context) -> void;
};
} // namespace ox
