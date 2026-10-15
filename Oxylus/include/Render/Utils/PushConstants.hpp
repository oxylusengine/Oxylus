#pragma once

#include <array>
#include <cstring>
#include <type_traits>

#include "Core/Types.hpp"

namespace ox {

template <usize ALIGNMENT, typename... T>
constexpr auto PushConstants_calc_size() -> usize {
  auto offset = 0_sz;
  ((offset = ox::align_up(offset, ALIGNMENT), offset += sizeof(T)), ...);
  return offset;
}

template <usize ALIGNMENT, typename... T>
constexpr auto PushConstants_calc_offsets() -> std::array<usize, sizeof...(T)> {
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

  auto data(this const PushConstants& self) -> const void* { return self.struct_data.data(); }

  auto size(this const PushConstants& self) -> usize { return self.struct_data.size(); }
};
} // namespace ox
