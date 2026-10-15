#pragma once

#include "Render/Graph/Fwd.hpp"
#include "Render/Graph/Types.hpp"

namespace ox {
enum class BufferID : u64 { Invalid = ~0_u64 };
enum class ImageID : u64 { Invalid = ~0_u64 };
enum class ImageViewID : u64 { Invalid = ~0_u64 };
enum class SamplerID : u64 { Invalid = ~0_u64 };
enum class PipelineID : u64 { Invalid = ~0_u64 };
enum class AccelerationStructureID : u64 { Invalid = ~0_u64 };
class RenderContext;
struct RenderResources;
struct SwapchainInfo;
} // namespace ox
