#pragma once

namespace ox {
struct HasherI {
  virtual ~HasherI() = default;

  virtual bool hash(const void* data, usize data_size) = 0;
  virtual u64 value() = 0;
  virtual void reset() = 0;
};

struct HasherXXH64 : HasherI {
  HasherXXH64();
  ~HasherXXH64() override;

  bool hash(const void* data, usize data_size) override;
  u64 value() override;
  void reset() override;

  void* handle = nullptr;
};

namespace detail {
constexpr u32 fnv32_val = 2166136261_u32;
constexpr u32 fnv32_prime = 16777619_u32;
constexpr u64 fnv64_val = 14695981039346656037_u64;
constexpr u64 fnv64_prime = 1099511628211_u64;
} // namespace detail

constexpr u32 fnv32(const c8* data, u32 data_size) {
  u32 fnv = detail::fnv32_val;

  for (u32 i = 0; i < data_size; i++)
    fnv = (fnv * detail::fnv32_prime) ^ data[i];

  return fnv;
}

template <typename T>
constexpr u32 fnv32(const T& val) {
  return fnv32(&val, sizeof(T));
}

constexpr u32 fnv32_str(std::string_view str) { return fnv32(str.data(), static_cast<u32>(str.length())); }

constexpr u64 fnv64(const c8* data, usize data_size) {
  u64 fnv = detail::fnv64_val;

  for (u32 i = 0; i < data_size; i++)
    fnv = (fnv * detail::fnv64_prime) ^ data[i];

  return fnv;
}

template <typename T>
constexpr u64 fnv64(const T& val) {
  return fnv64(&val, sizeof(T));
}

constexpr u64 fnv64_str(std::string_view str) { return fnv64(str.data(), str.length()); }

// COMPILE TIME
consteval u32 fnv32_c(std::string_view str) { return fnv32(str.data(), static_cast<u32>(str.length()));
}

// COMPILE TIME
consteval u64 fnv64_c(std::string_view str) { return fnv64(str.data(), str.length()); }

} // namespace ox
