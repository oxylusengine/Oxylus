#pragma once

#include "Core/Types.hpp"

namespace ox {
enum class BufferID : u64 { Invalid = ~0_u64 };
enum class ImageID : u64 { Invalid = ~0_u64 };
enum class ImageViewID : u64 { Invalid = ~0_u64 };
enum class SamplerID : u64 { Invalid = ~0_u64 };
enum class PipelineID : u64 { Invalid = ~0_u64 };

enum : u32 {
  DescriptorTable_SamplerIndex = 0,
  DescriptorTable_SampledImageIndex,
  DescriptorTable_StorageImageIndex,
};
} // namespace ox
