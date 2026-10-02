#pragma once

#include <vuk/Name.hpp>
#include <vuk/Types.hpp>

#include "Core/Types.hpp"
#include "Render/GPU/Shared.hpp"
#include "Utils/OxMath.hpp"

namespace ox::GPU {
enum class TransformID : u64 { Invalid = ~0_u64 };
enum class LightID : u64 { Invalid = ~0_u64 };

struct Transforms {
  glm::mat4 world = {};
  glm::mat4 previous_world = {};
};

constexpr auto ddgi_atlas_extent(u32 probe_count, u32 interior_texels) -> vuk::Extent3D {
  const auto tile = interior_texels + 2;
  const auto rows = (probe_count + DDGI_PROBES_PER_ATLAS_ROW - 1) / DDGI_PROBES_PER_ATLAS_ROW;
  return {.width = DDGI_PROBES_PER_ATLAS_ROW * tile, .height = rows * tile, .depth = 1};
}

static_assert(DDGI_IRRADIANCE_ATLAS_WIDTH % (DDGI_RADIANCE_TEXELS + 2) == 0);

constexpr auto ddgi_radiance_atlas_y_offset(u32 probe_count) -> u32 {
  return ddgi_atlas_extent(probe_count, DDGI_IRRADIANCE_TEXELS).height;
}

constexpr auto ddgi_irradiance_atlas_extent(u32 probe_count) -> vuk::Extent3D {
  const auto irradiance = ddgi_atlas_extent(probe_count, DDGI_IRRADIANCE_TEXELS);
  const auto radiance_rows = (probe_count + DDGI_RADIANCE_PROBES_PER_ATLAS_ROW - 1) /
                             DDGI_RADIANCE_PROBES_PER_ATLAS_ROW;
  return {
    .width = irradiance.width,
    .height = irradiance.height + radiance_rows * (DDGI_RADIANCE_TEXELS + 2),
    .depth = 1,
  };
}

constexpr static auto DDGI_MAX_IRRADIANCE_ATLAS_EXTENT = ddgi_irradiance_atlas_extent(DDGI_MAX_PROBE_COUNT);
static_assert(DDGI_MAX_IRRADIANCE_ATLAS_EXTENT.width <= DDGI_MAX_IMAGE_DIMENSION);
static_assert(DDGI_MAX_IRRADIANCE_ATLAS_EXTENT.height <= DDGI_MAX_IMAGE_DIMENSION);

constexpr auto ddgi_probes_per_ray_row(u32 rays_per_probe) -> u32 {
  auto probes = 1_u32;
  while (probes * 2 * rays_per_probe <= DDGI_MAX_IMAGE_DIMENSION) {
    probes *= 2;
  }
  return probes;
}

constexpr auto ddgi_ray_data_extent(u32 probe_count, u32 rays_per_probe) -> vuk::Extent3D {
  const auto probes_per_row = ddgi_probes_per_ray_row(rays_per_probe);
  const auto rows = (probe_count + probes_per_row - 1) / probes_per_row;
  return {.width = probes_per_row * rays_per_probe, .height = rows, .depth = 1};
}

static_assert(DDGI_TEXEL_UPDATE_THREADS_Y % DDGI_IRRADIANCE_TEXELS == 0);
static_assert(DDGI_TEXEL_UPDATE_THREADS_Y % DDGI_DISTANCE_TEXELS == 0);
static_assert(DDGI_TEXEL_UPDATE_THREADS_Y % DDGI_TEXEL_UPDATE_GROUP == 0);

constexpr static u32 HISTOGRAM_THREADS_X = 16;
constexpr static u32 HISTOGRAM_THREADS_Y = 16;
constexpr static u32 HISTOGRAM_BIN_COUNT = HISTOGRAM_THREADS_X * HISTOGRAM_THREADS_Y;

struct EyeAdaptationSettings {
  f32 min_exposure = -6.0f;
  f32 max_exposure = 18.0f;
  f32 adaptation_speed = 1.1f;
  f32 ev100_bias = 1.0f;
};

struct DrawBatch2D {
  vuk::Name pipeline_name = {};
  u32 offset = 0;
  u32 count = 0;
};

// A half's raw bits only compare correctly while it is positive: the sign bit makes every negative
// value look larger than every positive one. Flip so negatives order below positives.
constexpr auto sprite_half_sort_key(u32 bits) -> u32 { return (bits & 0x8000u) ? (~bits & 0xFFFFu) : (bits | 0x8000u); }

// y (when sorting by y) is the low priority half of the key, distance the high one
inline auto sprite_sort_key(const SpriteGPUData& sprite) -> u64 {
  const u64 distance_y = math::unpack_u32_low(sprite.flags16_distance16) & RENDER_FLAGS_2D_SORT_Y
                           ? sprite_half_sort_key(math::unpack_u32_high(sprite.material_id16_ypos16))
                           : 0u;
  const u64 distance_z = math::unpack_u32_high(sprite.flags16_distance16);
  return distance_y | (distance_z << 32u);
}

struct SpriteGreater {
  auto operator()(const SpriteGPUData& lhs, const SpriteGPUData& rhs) const -> bool {
    return sprite_sort_key(lhs) > sprite_sort_key(rhs);
  }
};

constexpr auto encode_particle_op(ParticleOpCode op, u32 dst_register, u32 write_mask) -> u32 {
  return static_cast<u32>(op) | (dst_register << 8u) | (write_mask << 12u);
}

constexpr auto encode_particle_operand(ParticleOperandKind kind, u32 payload) -> u32 {
  return (static_cast<u32>(kind) << 30u) | (payload & 0x3FFFFFFFu);
}

constexpr static u32 PARTICLE_ATTRIBUTE_REGISTERS = 5;

// The emitter program is a third bytecode program run once per emitter per frame, on the CPU, over
// the same instruction encoding. Its registers mean something else entirely.
constexpr static u32 PARTICLE_EMITTER_REG_OUTPUT = 0;  // x spawn count, y spawn rate
constexpr static u32 PARTICLE_EMITTER_REG_CONTEXT = 1; // x time, y delta time, z cycle norm, w queued pulses
constexpr static u32 PARTICLE_EMITTER_ATTRIBUTE_REGISTERS = 2;
constexpr static u32 PARTICLE_EMITTER_STATE_COUNT = 8;
} // namespace ox::GPU
