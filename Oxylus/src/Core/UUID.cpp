#include "UUID.hpp"

#include <random>

namespace ox {
thread_local std::random_device uuid_random_device;
thread_local std::mt19937_64 uuid_random_engine(uuid_random_device());
thread_local std::uniform_int_distribution<u64> uuid_uniform_dist;

constexpr bool is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

constexpr u8 hex_to_u8(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<u8>(c - '0');
  }

  return (c >= 'A' && c <= 'F') ? static_cast<u8>(c - 'A' + 10) : static_cast<u8>(c - 'a' + 10);
}

UUID UUID::generate_random() {
  ZoneScoped;

  UUID uuid;
  std::ranges::generate(uuid.m_data.arr, std::ref(uuid_random_device));
  uuid.m_data.u64x2[0] &= 0xffffffffffff0fff_u64;
  uuid.m_data.u64x2[0] |= 0x0000000000004000_u64;
  uuid.m_data.u64x2[1] &= 0x3fffffffffffffff_u64;
  uuid.m_data.u64x2[1] |= 0x8000000000000000_u64;

  return uuid;
}

option<UUID> UUID::from_string(std::string_view str) {
  ZoneScoped;

  if (str.size() != UUID::LENGTH || str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
    return nullopt;
  }

  auto convert_segment = [](std::string_view target, usize offset, usize length) -> option<u64> {
    u64 val = 0;
    for (usize i = 0; i < length; i++) {
      auto c = target[offset + i];
      if (!is_hex_digit(c)) {
        return nullopt;
      }

      val = (val << 4) | hex_to_u8(c);
    }

    return val;
  };

  UUID uuid = {};
  uuid.m_data.u64x2[0] = (convert_segment(str, 0, 8).value() << 32) |  //
                         (convert_segment(str, 9, 4).value() << 16) |  //
                         convert_segment(str, 14, 4).value();
  uuid.m_data.u64x2[1] = (convert_segment(str, 19, 4).value() << 48) | //
                         convert_segment(str, 24, 12).value();

#ifdef OX_DEBUG
  uuid.debug = uuid.str();
#endif

  return uuid;
}

std::string UUID::str() const {
  ZoneScoped;

  return fmt::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
                     static_cast<u32>(this->m_data.u64x2[0] >> 32_u64),
                     static_cast<u32>((this->m_data.u64x2[0] >> 16_u64) & 0x0000ffff_u64),
                     static_cast<u32>(this->m_data.u64x2[0] & 0x0000ffff_u64),
                     static_cast<u32>(this->m_data.u64x2[1] >> 48_u64),
                     this->m_data.u64x2[1] & 0x0000ffffffffffff_u64);
}

} // namespace ox
