#pragma once

#include <ankerl/unordered_dense.h>
#include <span>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct UUID {
private:
  union {
    u64 u64x2[2] = {};
    u8 u8x16[16];
    std::array<u8, 16> arr;
  } data = {};

#ifdef OX_DEBUG
  std::string debug = {};
#endif

public:
  constexpr static size_t LENGTH = 36;

  static auto generate_random() -> UUID;
  static auto from_string(std::string_view str) -> option<UUID>;
  static auto from_bytes(std::span<u8> bytes) -> option<UUID>;

  UUID() = default;
  explicit UUID(nullptr_t) {}
  UUID(const UUID& other) = default;
  UUID& operator=(const UUID& other) = default;
  UUID(UUID&& other) = default;
  UUID& operator=(UUID&& other) = default;

  std::string str() const;
  std::span<const u8, 16> bytes() const { return data.arr; }
  constexpr bool operator==(const UUID& other) const {
    return data.u64x2[0] == other.data.u64x2[0] && data.u64x2[1] == other.data.u64x2[1];
  }
  explicit operator bool() const { return data.u64x2[0] != 0 && data.u64x2[1] != 0; }
};
} // namespace ox

template <>
struct ankerl::unordered_dense::hash<ox::UUID> {
  using is_avalanching = void;
  auto operator()(const ox::UUID& uuid) const noexcept {
    const auto& v = uuid.bytes();
    return ankerl::unordered_dense::detail::wyhash::hash(v.data(), v.size() * sizeof(ox::u8));
  }
};
