#pragma once

#include <ankerl/unordered_dense.h>
#include <shared_mutex>
#include <variant>

#include "Memory/SlotMap.hpp"
#include "Render/Fwd.hpp"
#include "Render/Graph/Resource.hpp"
#include "Render/RenderContext.hpp"

namespace ox {
struct DescriptorSet {
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  std::vector<VkDescriptorSetLayoutBinding> bindings = {};
  std::vector<VkDescriptorBindingFlags> flags = {};
};

struct RenderPipelineNameHash {
  using is_transparent = void;
  using is_avalanching = void;
  auto operator()(this const RenderPipelineNameHash&, std::string_view name) noexcept -> u64 {
    return ankerl::unordered_dense::hash<std::string_view>{}(name);
  }
};

struct RetiredRenderResource {
  std::variant<vir::Buffer, vir::Image, vir::ImageView, vir::Sampler, GPUAccelerationStructure> value = {};
  std::variant<BufferID, ImageID, ImageViewID, SamplerID, AccelerationStructureID> id = BufferID::Invalid;
  vir::SubmitTicket ticket = {};
  // a frame recording when this was destroyed may still name it, so the ticket waits for that frame
  bool is_sealed = false;
};

struct RenderResources {
  SlotMap<vir::Buffer, BufferID> buffers = {};
  SlotMap<vir::Image, ImageID> images = {};
  SlotMap<vir::ImageView, ImageViewID> image_views = {};
  SlotMap<vir::Sampler, SamplerID> samplers = {};
  SlotMap<GPUAccelerationStructure, AccelerationStructureID> acceleration_structures = {};
  SlotMap<vir::PipelineID, PipelineID> pipelines = {};
  ankerl::unordered_dense::map<std::string, PipelineID, RenderPipelineNameHash, std::equal_to<>> pipeline_names = {};
  mutable std::shared_mutex pipeline_mutex = {};
  DescriptorSet descriptor_set = {};
  vir::SwapChain swapchain = {};
  u64 swapchain_generation = 1;
  std::vector<std::pair<vir::SamplerInfo, SamplerID>> cached_samplers = {};
  mutable std::shared_mutex sampler_cache_mutex = {};
  mutable std::shared_mutex retirement_mutex = {};
  std::vector<RetiredRenderResource> retired = {};
};
} // namespace ox
