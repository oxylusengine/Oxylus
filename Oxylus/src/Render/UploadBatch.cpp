#include "Render/UploadBatch.hpp"

#include <ranges>
#include <vuk/runtime/vk/PipelineInstance.hpp>
#include <vuk/runtime/vk/Query.hpp>

#include "Core/App.hpp"
#include "Memory/Stack.hpp"
#include "Render/RenderContext.hpp"

namespace ox {
UploadBatch::~UploadBatch() {
  ZoneScoped;

  // Freeing a staging buffer with its copy still in flight is a use-after-free, so a batch that was
  // never flushed has to settle here.
  if (!submitted.empty()) {
    flush(App::get_rendercontext());
  }
}

auto UploadBatch::create() -> Arc<UploadBatch> { return Arc<UploadBatch>::create(); }

auto UploadBatch::add_upload(this UploadBatch& self, std::span<vuk::UntypedValue> values) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.mutex);
  self.submitted.reserve(self.submitted.size() + values.size());
  for (auto& value : values) {
    self.submitted.emplace_back(std::move(value));
  }
}

auto UploadBatch::take_staging(this UploadBatch& self, std::span<vuk::Unique<vuk::Buffer>> buffers) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.mutex);
  self.staging.reserve(self.staging.size() + buffers.size());
  for (auto& buffer : buffers) {
    self.staging.emplace_back(std::move(buffer));
  }
}

auto UploadBatch::add_descriptor_write(this UploadBatch& self, const DescriptorWrite& write) -> void {
  ZoneScoped;

  auto lock = std::unique_lock(self.mutex);
  self.descriptor_writes.emplace_back(write);
}

auto UploadBatch::flush(this UploadBatch& self, RenderContext& render_context) -> void {
  ZoneScoped;
  memory::ScopedStack stack;

  auto lock = std::unique_lock(self.mutex);

  if (!self.submitted.empty()) {
    // Everything here was already submitted, so this compiles nothing and only waits on the
    // collected sync points.
    render_context.wait_on_multiple(self.submitted);
  }

  if (!self.descriptor_writes.empty()) {
    const auto& bindless_set = render_context.get_descriptor_set();
    auto writes = stack.alloc<VkWriteDescriptorSet>(self.descriptor_writes.size());
    for (const auto& [write, out] : std::views::zip(self.descriptor_writes, writes)) {
      out = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = bindless_set.backing_set,
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

  self.submitted.clear();
  self.staging.clear();
  self.descriptor_writes.clear();
}
} // namespace ox
