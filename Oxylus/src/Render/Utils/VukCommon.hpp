#pragma once

#include <vuk/Value.hpp>
#include <vuk/vsl/Core.hpp>

namespace ox {

template <usize ALIGNMENT,
          typename... T>
constexpr auto PushConstants_calc_size() -> usize {
  auto offset = 0_sz;
  ((offset = ox::align_up(offset, ALIGNMENT), offset += sizeof(T)), ...);
  return offset;
}

template <usize ALIGNMENT,
          typename... T>
constexpr auto PushConstants_calc_offsets() {
  auto offsets = std::array<usize, sizeof...(T)>{};
  auto offset = 0_sz;
  auto index = 0_sz;
  ((offsets[index++] = (offset = ox::align_up(offset, ALIGNMENT), offset), offset += sizeof(T)), ...);
  return offsets;
}

template <typename... T>
struct PushConstants {
  static_assert((std::is_trivially_copyable_v<T> && ...));
  constexpr static usize ALIGNMENT = 4;
  constexpr static usize TOTAL_SIZE = PushConstants_calc_size<ALIGNMENT, T...>();
  constexpr static auto MEMBER_OFFSETS = PushConstants_calc_offsets<ALIGNMENT, T...>();
  std::array<u8, TOTAL_SIZE> struct_data = {};

  PushConstants(T... args) {
    auto index = 0_sz;
    ((std::memcpy(struct_data.data() + MEMBER_OFFSETS[index++], &args, sizeof(T))), ...);
  }

  auto data() const -> void* { return struct_data.data(); }

  auto size() const -> usize { return struct_data.size(); }
};
} // namespace ox

namespace vuk {
inline SamplerCreateInfo NearestSamplerClamped = {
    .magFilter = Filter::eNearest,
    .minFilter = Filter::eNearest,
    .mipmapMode = SamplerMipmapMode::eNearest,
    .addressModeU = SamplerAddressMode::eClampToEdge,
    .addressModeV = SamplerAddressMode::eClampToEdge,
    .addressModeW = SamplerAddressMode::eClampToEdge,
};

inline SamplerCreateInfo NearestSamplerRepeated = {
    .magFilter = Filter::eNearest,
    .minFilter = Filter::eNearest,
    .mipmapMode = SamplerMipmapMode::eNearest,
    .addressModeU = SamplerAddressMode::eRepeat,
    .addressModeV = SamplerAddressMode::eRepeat,
    .addressModeW = SamplerAddressMode::eRepeat,
};

inline SamplerCreateInfo NearestMagLinearMinSamplerClamped = {
    .magFilter = Filter::eLinear,
    .minFilter = Filter::eNearest,
    .mipmapMode = SamplerMipmapMode::eNearest,
    .addressModeU = SamplerAddressMode::eClampToEdge,
    .addressModeV = SamplerAddressMode::eClampToEdge,
    .addressModeW = SamplerAddressMode::eClampToEdge,
};

inline SamplerCreateInfo LinearMipmapNearestSamplerClamped = {
    .magFilter = Filter::eNearest,
    .minFilter = Filter::eNearest,
    .mipmapMode = SamplerMipmapMode::eLinear,
    .addressModeU = SamplerAddressMode::eClampToEdge,
    .addressModeV = SamplerAddressMode::eClampToEdge,
    .addressModeW = SamplerAddressMode::eClampToEdge,
};

inline SamplerCreateInfo LinearSamplerRepeated = {
    .magFilter = Filter::eLinear,
    .minFilter = Filter::eLinear,
    .mipmapMode = SamplerMipmapMode::eLinear,
    .addressModeU = SamplerAddressMode::eRepeat,
    .addressModeV = SamplerAddressMode::eRepeat,
    .addressModeW = SamplerAddressMode::eRepeat,
};

inline SamplerCreateInfo LinearSamplerRepeatedAnisotropy = {
    .magFilter = Filter::eLinear,
    .minFilter = Filter::eLinear,
    .mipmapMode = SamplerMipmapMode::eLinear,
    .addressModeU = SamplerAddressMode::eRepeat,
    .addressModeV = SamplerAddressMode::eRepeat,
    .addressModeW = SamplerAddressMode::eRepeat,
    .anisotropyEnable = true,
    .maxAnisotropy = 16.0f,
};

inline SamplerCreateInfo LinearSamplerClamped = {
    .magFilter = Filter::eLinear,
    .minFilter = Filter::eLinear,
    .mipmapMode = SamplerMipmapMode::eLinear,
    .addressModeU = SamplerAddressMode::eClampToEdge,
    .addressModeV = SamplerAddressMode::eClampToEdge,
    .addressModeW = SamplerAddressMode::eClampToEdge,
    .borderColor = BorderColor::eFloatOpaqueWhite,
};

inline SamplerCreateInfo CmpDepthSampler = {
    .magFilter = Filter::eLinear,
    .minFilter = Filter::eLinear,
    .mipmapMode = SamplerMipmapMode::eNearest,
    .addressModeU = SamplerAddressMode::eClampToEdge,
    .addressModeV = SamplerAddressMode::eClampToEdge,
    .addressModeW = SamplerAddressMode::eClampToEdge,
    .compareEnable = true,
    .compareOp = CompareOp::eGreaterOrEqual,
    .minLod = 0.0f,
    .maxLod = 0.0f,
};

inline vuk::Extent3D operator/(const vuk::Extent3D& ext,
                               float rhs) {
  return {unsigned((float)ext.width / rhs), unsigned((float)ext.height / rhs), 1u};
}

vuk::Value<vuk::ImageAttachment> generate_mips(vuk::Value<vuk::ImageAttachment> image,
                                               uint32_t mip_count);

inline VkDescriptorSetLayoutBinding ds_layout_binding(uint32_t binding,
                                                      vuk::DescriptorType descriptor_type,
                                                      const uint32_t count = 1024) {
  return {
      .binding = binding,
      .descriptorType = static_cast<VkDescriptorType>(descriptor_type),
      .descriptorCount = count,
      .stageFlags = static_cast<VkShaderStageFlags>(vuk::ShaderStageFlagBits::eAll),
      .pImmutableSamplers = nullptr,
  };
}

inline vuk::DescriptorSetLayoutCreateInfo
descriptor_set_layout_create_info(const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                                  const uint32_t index) {
  vuk::DescriptorSetLayoutCreateInfo ci = {};
  ci.bindings = bindings;
  ci.index = index;
  for ([[maybe_unused]] const auto& _ : bindings)
    ci.flags.emplace_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
  return ci;
}
} // namespace vuk
