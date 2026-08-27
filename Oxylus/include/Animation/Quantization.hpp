#pragma once

#include <glm/gtx/quaternion.hpp>

#include "Animation/Fwd.hpp"

// rotations are smallest-three in 48 bits, translation and scale ride explicit per-track ranges at
// 16 bits per component
namespace ox::animation {
constexpr static auto UNSIGNED_14_MAX = 16383.f;
constexpr static auto UNSIGNED_16_MAX = 65535.f;
constexpr static auto QUAT_COMPONENT_RANGE = 0.70710678118654752440f; // 1/sqrt(2)

// sign in bit 0, 14-bit magnitude in bits 1..14, sign-magnitude rather than two's complement so
// the encoding is symmetric around zero and a zero component stays exactly zero
inline auto encode_signed_normalized_15(const f32 value) -> u16 {
  const auto sign = value < 0.f ? 1_u32 : 0_u32;
  const auto magnitude = static_cast<u32>(glm::min(glm::abs(value), 1.f) * UNSIGNED_14_MAX + 0.5f);
  return static_cast<u16>(sign | (magnitude << 1));
}

inline auto decode_signed_normalized_15(const u16 encoded) -> f32 {
  const auto magnitude = static_cast<f32>((encoded >> 1) & 0x3FFF) / UNSIGNED_14_MAX;
  return (encoded & 1) ? -magnitude : magnitude;
}

struct FloatRange {
  f32 start = 0.f;
  f32 length = 0.f;

  static auto from_bounds(const f32 min_value, const f32 max_value) -> FloatRange {
    return FloatRange{.start = min_value, .length = max_value - min_value};
  }

  auto is_static(this const FloatRange& self) -> bool { return self.length <= 0.f; }

  auto encode(this const FloatRange& self, const f32 value) -> u16 {
    if (self.length <= 0.f) {
      return 0;
    }

    const auto normalized = glm::clamp((value - self.start) / self.length, 0.f, 1.f);
    return static_cast<u16>(normalized * UNSIGNED_16_MAX + 0.5f);
  }

  auto decode(this const FloatRange& self, const u16 encoded) -> f32 {
    return self.start + (static_cast<f32>(encoded) / UNSIGNED_16_MAX) * self.length;
  }
};

struct EncodedQuaternion {
  u16 data0 = 0;
  u16 data1 = 0;
  u16 data2 = 0;

  static auto encode(const glm::quat& quat) -> EncodedQuaternion {
    const f32 components[4] = {quat.x, quat.y, quat.z, quat.w};

    auto largest_index = 0_u32;
    for (auto i = 1_u32; i < 4; ++i) {
      if (glm::abs(components[i]) > glm::abs(components[largest_index])) {
        largest_index = i;
      }
    }

    // q and -q are the same rotation, so forcing the dropped component positive lets it be
    // recovered from the magnitude of the other three alone
    const auto sign = components[largest_index] < 0.f ? -1.f : 1.f;

    f32 rest[3] = {};
    for (auto i = 0_u32, r = 0_u32; i < 4; ++i) {
      if (i != largest_index) {
        rest[r++] = components[i] * sign / QUAT_COMPONENT_RANGE;
      }
    }

    return EncodedQuaternion{
      .data0 = static_cast<u16>(((largest_index >> 1) << 15) | encode_signed_normalized_15(rest[0])),
      .data1 = static_cast<u16>(((largest_index & 1) << 15) | encode_signed_normalized_15(rest[1])),
      .data2 = encode_signed_normalized_15(rest[2]),
    };
  }

  auto decode(this const EncodedQuaternion& self) -> glm::quat {
    const auto largest_index = static_cast<u32>(((self.data0 >> 15) << 1) | (self.data1 >> 15));

    const auto a = decode_signed_normalized_15(self.data0 & 0x7FFF) * QUAT_COMPONENT_RANGE;
    const auto b = decode_signed_normalized_15(self.data1 & 0x7FFF) * QUAT_COMPONENT_RANGE;
    const auto c = decode_signed_normalized_15(self.data2 & 0x7FFF) * QUAT_COMPONENT_RANGE;
    const auto largest = glm::sqrt(glm::max(0.f, 1.f - (a * a + b * b + c * c)));

    switch (largest_index) {
      case 0 : return glm::normalize(glm::quat::wxyz(c, largest, a, b));
      case 1 : return glm::normalize(glm::quat::wxyz(c, a, largest, b));
      case 2 : return glm::normalize(glm::quat::wxyz(c, a, b, largest));
      default: return glm::normalize(glm::quat::wxyz(largest, a, b, c));
    }
  }
};
} // namespace ox::animation
