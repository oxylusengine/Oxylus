#pragma once

#include <memory>
#include <shared_mutex>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "Core/Arc.hpp"
#include "Core/Types.hpp"
#include "Render/Graph/Fwd.hpp"
#include "Render/Graph/Resource.hpp"

namespace ox {
class RenderContext;

// uploads submitted from loader jobs, settled together so a model waits on one batch
struct UploadBatch : ManagedObj {
  struct DescriptorWrite {
    u32 binding = 0;
    u32 array_element = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
    VkDescriptorImageInfo image_info = {};
  };

  // the allocator holds the command buffers and transients of the upload until its ticket retires
  struct Pending {
    std::unique_ptr<vir::FrameAllocator> allocator = {};
    std::vector<vir::Buffer> staging = {};
    vir::SubmitTicket ticket = {};
  };

  std::shared_mutex mutex = {};
  std::vector<Pending> pending = {};
  std::vector<DescriptorWrite> descriptor_writes = {};

  UploadBatch();
  ~UploadBatch();

  static auto create() -> Arc<UploadBatch>;

  auto add_upload(this UploadBatch& self, Pending&& upload) -> void;
  auto add_descriptor_write(this UploadBatch& self, const DescriptorWrite& write) -> void;

  auto flush(this UploadBatch& self, RenderContext& render_context) -> void;
};
} // namespace ox
