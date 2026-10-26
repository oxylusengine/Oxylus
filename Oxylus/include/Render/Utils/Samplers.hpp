#pragma once

#include <vir/Resource.hpp>

namespace ox {
inline const vir::SamplerInfo NearestSamplerClamped = {
  .mag_filter = VK_FILTER_NEAREST,
  .min_filter = VK_FILTER_NEAREST,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
};

inline const vir::SamplerInfo NearestSamplerRepeated = {
  .mag_filter = VK_FILTER_NEAREST,
  .min_filter = VK_FILTER_NEAREST,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT,
};

inline const vir::SamplerInfo NearestMagLinearMinSamplerClamped = {
  .mag_filter = VK_FILTER_LINEAR,
  .min_filter = VK_FILTER_NEAREST,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
};

inline const vir::SamplerInfo LinearMipmapNearestSamplerClamped = {
  .mag_filter = VK_FILTER_NEAREST,
  .min_filter = VK_FILTER_NEAREST,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
};

inline const vir::SamplerInfo LinearSamplerRepeated = {
  .mag_filter = VK_FILTER_LINEAR,
  .min_filter = VK_FILTER_LINEAR,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT,
};

inline const vir::SamplerInfo LinearSamplerRepeatedAnisotropy = {
  .mag_filter = VK_FILTER_LINEAR,
  .min_filter = VK_FILTER_LINEAR,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT,
  .max_anisotropy = 16.0f,
};

inline const vir::SamplerInfo LinearSamplerClamped = {
  .mag_filter = VK_FILTER_LINEAR,
  .min_filter = VK_FILTER_LINEAR,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
};

inline const vir::SamplerInfo LinearSamplerBorder = {
  .mag_filter = VK_FILTER_LINEAR,
  .min_filter = VK_FILTER_LINEAR,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
  .border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
};

inline const vir::SamplerInfo CmpDepthSampler = {
  .mag_filter = VK_FILTER_LINEAR,
  .min_filter = VK_FILTER_LINEAR,
  .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
  .address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
  .compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL,
  .min_lod = 0.0f,
  .max_lod = 0.0f,
};

} // namespace ox
