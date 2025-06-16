#pragma once

namespace ox {
struct UUID {
private:
  union {
    u64 u64x2[2] = {};
    u8 u8x16[16];
    std::array<u8, 16> arr;
  } m_data = {};

#ifdef OX_DEBUG
  std::string debug = {};
#endif

public:
  constexpr static size_t LENGTH = 36;

  static UUID generate_random();
  static option<UUID> from_string(std::string_view str);

  UUID() = default;
  explicit UUID(nullptr_t) {}
  UUID(const UUID& other) = default;
  UUID& operator=(const UUID& other) = default;
  UUID(UUID&& other) = default;
  UUID& operator=(UUID&& other) = default;

  std::string str() const;
  const std::array<u8, 16> bytes() const { return m_data.arr; }
  std::array<u8, 16> bytes() { return m_data.arr; }
  constexpr bool operator==(const UUID& other) const {
    return m_data.u64x2[0] == other.m_data.u64x2[0] && m_data.u64x2[1] == other.m_data.u64x2[1];
  }
  explicit operator bool() const { return m_data.u64x2[0] != 0 && m_data.u64x2[1] != 0; }
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
