#include "Render/Graph/Vulkan.hpp"

#include <cstddef>
#include <type_traits>

#include "Render/Graph/Types.hpp"
#include "Utils/Log.hpp"

namespace ox::vir {
// value conversions at native boundaries depend on these layouts
static_assert(sizeof(Extent2D) == sizeof(VkExtent2D));
static_assert(alignof(Extent2D) == alignof(VkExtent2D));
static_assert(std::is_trivially_copyable_v<Extent2D> && std::is_trivially_copyable_v<VkExtent2D>);
static_assert(std::is_standard_layout_v<Extent2D> && std::is_aggregate_v<Extent2D>);
static_assert(offsetof(Extent2D, width) == offsetof(VkExtent2D, width));
static_assert(sizeof(Extent2D::width) == sizeof(VkExtent2D::width));
static_assert(offsetof(Extent2D, height) == offsetof(VkExtent2D, height));
static_assert(sizeof(Extent2D::height) == sizeof(VkExtent2D::height));
static_assert(sizeof(Extent3D) == sizeof(VkExtent3D));
static_assert(alignof(Extent3D) == alignof(VkExtent3D));
static_assert(std::is_trivially_copyable_v<Extent3D> && std::is_trivially_copyable_v<VkExtent3D>);
static_assert(std::is_standard_layout_v<Extent3D> && std::is_aggregate_v<Extent3D>);
static_assert(offsetof(Extent3D, width) == offsetof(VkExtent3D, width));
static_assert(sizeof(Extent3D::width) == sizeof(VkExtent3D::width));
static_assert(offsetof(Extent3D, height) == offsetof(VkExtent3D, height));
static_assert(sizeof(Extent3D::height) == sizeof(VkExtent3D::height));
static_assert(offsetof(Extent3D, depth) == offsetof(VkExtent3D, depth));
static_assert(sizeof(Extent3D::depth) == sizeof(VkExtent3D::depth));
static_assert(sizeof(ComponentMapping) == sizeof(VkComponentMapping));
static_assert(alignof(ComponentMapping) == alignof(VkComponentMapping));
static_assert(std::is_trivially_copyable_v<ComponentMapping> && std::is_trivially_copyable_v<VkComponentMapping>);
static_assert(std::is_standard_layout_v<ComponentMapping> && std::is_aggregate_v<ComponentMapping>);
static_assert(offsetof(ComponentMapping, r) == offsetof(VkComponentMapping, r));
static_assert(sizeof(ComponentMapping::r) == sizeof(VkComponentMapping::r));
static_assert(offsetof(ComponentMapping, g) == offsetof(VkComponentMapping, g));
static_assert(sizeof(ComponentMapping::g) == sizeof(VkComponentMapping::g));
static_assert(offsetof(ComponentMapping, b) == offsetof(VkComponentMapping, b));
static_assert(sizeof(ComponentMapping::b) == sizeof(VkComponentMapping::b));
static_assert(offsetof(ComponentMapping, a) == offsetof(VkComponentMapping, a));
static_assert(sizeof(ComponentMapping::a) == sizeof(VkComponentMapping::a));
static_assert(sizeof(Format) == sizeof(VkFormat) && alignof(Format) == alignof(VkFormat));
static_assert(
  sizeof(ImageCreateFlags) == sizeof(VkImageCreateFlags) && alignof(ImageCreateFlags) == alignof(VkImageCreateFlags)
);
static_assert(
  sizeof(ImageUsageFlags) == sizeof(VkImageUsageFlags) && alignof(ImageUsageFlags) == alignof(VkImageUsageFlags)
);
static_assert(
  sizeof(ImageViewCreateFlags) == sizeof(VkImageViewCreateFlags) &&
  alignof(ImageViewCreateFlags) == alignof(VkImageViewCreateFlags)
);
static_assert(sizeof(ImageType) == sizeof(VkImageType) && alignof(ImageType) == alignof(VkImageType));
static_assert(sizeof(ImageViewType) == sizeof(VkImageViewType) && alignof(ImageViewType) == alignof(VkImageViewType));
static_assert(
  sizeof(ComponentSwizzle) == sizeof(VkComponentSwizzle) && alignof(ComponentSwizzle) == alignof(VkComponentSwizzle)
);
static_assert(sizeof(Samples) == sizeof(VkSampleCountFlagBits) && alignof(Samples) == alignof(VkSampleCountFlagBits));

auto VulkanFunctions::load(
  this VulkanFunctions& self, PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance, VkDevice device
) -> bool {
  self.vkGetInstanceProcAddr = get_instance_proc;

  auto is_complete = true;
#define OX_VK_LOAD_INSTANCE(name)                                                                                      \
  self.name = reinterpret_cast<PFN_##name>(get_instance_proc(instance, #name));                                        \
  if (!self.name) {                                                                                                    \
    OX_LOG_ERROR("Vulkan instance function {} is missing", #name);                                                     \
    is_complete = false;                                                                                               \
  }
  OX_VK_INSTANCE_FUNCTIONS(OX_VK_LOAD_INSTANCE)
#undef OX_VK_LOAD_INSTANCE

  if (!self.vkGetDeviceProcAddr) {
    return false;
  }

#define OX_VK_LOAD_DEVICE(name)                                                                                        \
  self.name = reinterpret_cast<PFN_##name>(self.vkGetDeviceProcAddr(device, #name));                                   \
  if (!self.name) {                                                                                                    \
    OX_LOG_ERROR("Vulkan device function {} is missing", #name);                                                       \
    is_complete = false;                                                                                               \
  }
  OX_VK_DEVICE_FUNCTIONS(OX_VK_LOAD_DEVICE)
#undef OX_VK_LOAD_DEVICE

#define OX_VK_LOAD_OPTIONAL(name) self.name = reinterpret_cast<PFN_##name>(self.vkGetDeviceProcAddr(device, #name));
  OX_VK_OPTIONAL_DEVICE_FUNCTIONS(OX_VK_LOAD_OPTIONAL)
#undef OX_VK_LOAD_OPTIONAL

  return is_complete;
}
} // namespace ox::vir
