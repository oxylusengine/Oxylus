#pragma once

#include <utility>

#include "Core/Types.hpp"
#include "Render/Graph/Types.hpp"

namespace ox::vir {
enum class ValueID : u32 { Invalid = ~0_u32 };
enum class LabelID : u32 { Invalid = ~0_u32 };
enum class PipelineID : u32 { Invalid = ~0_u32 };
enum class QueueID : u8 { Invalid = 255 };

constexpr auto is_valid(ValueID id) -> bool { return id != ValueID::Invalid; }
constexpr auto is_valid(PipelineID id) -> bool { return id != PipelineID::Invalid; }
constexpr auto index_of(ValueID id) -> usize { return std::to_underlying(id); }

struct SubmitTicket {
  QueueID queue = QueueID::Invalid;
  u64 value = 0;

  auto operator<=>(const SubmitTicket&) const = default;
};

struct Context;
struct RenderGraph;
struct Module;
struct Program;
struct Recorder;
struct PersistentAllocator;
struct FrameAllocator;
struct SuperFrameAllocator;
struct Buffer;
struct Image;
struct ImageAttachment;
struct ImageInfo;
struct ImageView;
struct ImageViewInfo;
struct Sampler;
struct SamplerInfo;
struct BufferInfo;
struct SwapChain;
struct BindlessDescriptorSet;
struct RenderGraphCallbacks;

// bit operations for the flag enums below, which the engine-wide bitmask operators can't serve
// because their `&` collapses to bool
#define OX_VIR_FLAGS(T)                                                                                                \
  constexpr auto operator|(T lhs, T rhs)->T {                                                                          \
    return static_cast<T>(std::to_underlying(lhs) | std::to_underlying(rhs));                                          \
  }                                                                                                                    \
  constexpr auto operator&(T lhs, T rhs)->T {                                                                          \
    return static_cast<T>(std::to_underlying(lhs) & std::to_underlying(rhs));                                          \
  }                                                                                                                    \
  constexpr auto operator^(T lhs, T rhs)->T {                                                                          \
    return static_cast<T>(std::to_underlying(lhs) ^ std::to_underlying(rhs));                                          \
  }                                                                                                                    \
  constexpr auto operator~(T value)->T { return static_cast<T>(~std::to_underlying(value)); }                          \
  constexpr auto operator|=(T& lhs, T rhs)->T& { return lhs = lhs | rhs; }                                             \
  constexpr auto operator&=(T& lhs, T rhs)->T& { return lhs = lhs & rhs; }                                             \
  constexpr auto contains(T value, T flags) -> bool { return (value & flags) == flags; }                               \
  constexpr auto intersects(T value, T flags) -> bool { return std::to_underlying(value & flags) != 0; }               \
  constexpr auto is_empty(T value) -> bool { return std::to_underlying(value) == 0; }                                  \
  constexpr auto difference(T value, T flags) -> T { return value & ~flags; }

OX_VIR_FLAGS(ShaderStage)
} // namespace ox::vir
