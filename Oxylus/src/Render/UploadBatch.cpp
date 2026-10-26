#include "Render/UploadBatch.hpp"

#include <ranges>
#include <vir/Allocator.hpp>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"
#include "Render/RenderResources.hpp"
#include "Utils/Log.hpp"

namespace ox {
UploadBatch::UploadBatch() = default;

UploadBatch::~UploadBatch() {
  ZoneScoped;

  // freeing a staging buffer with its copy still in flight is a use-after-free, so a batch that was
  // never flushed has to settle here
  if (!pending.empty() || !descriptor_writes.empty()) {
    flush(App::get_rendercontext());
  }
}

auto UploadBatch::create() -> Arc<UploadBatch> { return Arc<UploadBatch>::create(); }

auto UploadBatch::add_upload(this UploadBatch& self, Pending&& upload) -> void {
  auto lock = std::unique_lock(self.mutex);
  self.pending.push_back(std::move(upload));
}

auto UploadBatch::add_descriptor_write(this UploadBatch& self, const DescriptorWrite& write) -> void {
  auto lock = std::unique_lock(self.mutex);
  self.descriptor_writes.push_back(write);
}

auto UploadBatch::flush(this UploadBatch& self, RenderContext& render_context) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto lock = std::unique_lock(self.mutex);

  for (auto& upload : self.pending) {
    if (const auto waited = render_context.wait(upload.ticket); !waited) {
      OX_LOG_ERROR("failed to wait for an upload ({})", std::to_underlying(waited.error()));
    }

    upload.allocator.reset();
    for (const auto& buffer : upload.staging) {
      render_context.persistent->deallocate_buffer(buffer);
    }
  }

  if (!self.descriptor_writes.empty()) {
    const auto set = render_context.resources->descriptor_set.set;
    auto writes = stack.alloc<VkWriteDescriptorSet>(self.descriptor_writes.size());
    for (const auto& [write, out] : std::views::zip(self.descriptor_writes, writes)) {
      out = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = write.binding,
        .dstArrayElement = write.array_element,
        .descriptorCount = 1,
        .descriptorType = write.type,
        .pImageInfo = &write.image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
      };
    }

    render_context.commit_descriptor_set(writes);
  }

  self.pending.clear();
  self.descriptor_writes.clear();
}
} // namespace ox
