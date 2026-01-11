#pragma once

#include <cstring>
#include <span>
#include <type_traits>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct BufferWriter {
  BufferWriter(void* buffer_, usize length) : buffer(std::span{static_cast<u8*>(buffer_), length}), offset(0) {}

  BufferWriter(std::span<u8> buffer_) : buffer(buffer_), offset(0) {}

  template <typename T>
  auto write(this BufferWriter& self, const T& value) -> bool {
    static_assert(std::is_trivially_copyable_v<T>);

    if (self.offset + sizeof(T) > self.buffer.size()) {
      return false;
    }

    std::memcpy(self.buffer.data() + self.offset, &value, sizeof(T));
    self.offset += sizeof(T);
    return true;
  }

  auto write_bytes(this BufferWriter& self, const void* data, usize size) -> bool {
    if (!data || self.offset + size > self.buffer.size()) {
      return false;
    }

    std::memcpy(self.buffer.data() + self.offset, data, size);
    self.offset += size;
    return true;
  }

  auto write_span(this BufferWriter& self, std::span<const u8> data) -> bool {
    if (self.offset + data.size() > self.buffer.size()) {
      return false;
    }

    std::memcpy(self.buffer.data() + self.offset, data.data(), data.size());
    self.offset += data.size();
    return true;
  }

  auto data(this const BufferWriter& self) -> const u8* { return self.buffer.data(); }

  auto size(this const BufferWriter& self) -> usize { return self.offset; }

  auto remaining(this const BufferWriter& self) -> usize { return self.buffer.size() - self.offset; }

  auto reset(this BufferWriter& self) -> void { self.offset = 0; }

  auto skip(this BufferWriter& self, usize skip_by) -> void { self.offset += skip_by; }

  std::span<u8> buffer;
  usize offset;
};

class BufferReader {
public:
  BufferReader(const void* buffer_, usize length)
      : buffer(std::span{static_cast<const u8*>(buffer_), length}),
        offset(0) {}

  BufferReader(std::span<const u8> buffer_) : buffer(buffer_), offset(0) {}

  template <typename T>
  auto read(this BufferReader& self) -> option<T> {
    static_assert(std::is_trivially_copyable_v<T>);

    if (self.offset + sizeof(T) > self.buffer.size()) {
      return nullopt;
    }

    T value;
    std::memcpy(&value, self.buffer.data() + self.offset, sizeof(T));
    self.offset += sizeof(T);
    return value;
  }

  auto read_bytes(this BufferReader& self, void* data, usize size) -> bool {
    if (!data || self.offset + size > self.buffer.size()) {
      return false;
    }

    std::memcpy(data, self.buffer.data() + self.offset, size);
    self.offset += size;
    return true;
  }

  auto read_span(this BufferReader& self, usize size) -> option<std::span<const u8>> {
    if (self.offset + size > self.buffer.size()) {
      return nullopt;
    }

    auto result = self.buffer.subspan(self.offset, size);
    self.offset += size;
    return result;
  }

  auto get_offset(this const BufferReader& self) -> usize { return self.offset; }

  auto remaining(this const BufferReader& self) -> usize { return self.buffer.size() - self.offset; }

  auto eof(this const BufferReader& self) -> bool { return self.offset >= self.buffer.size(); }

  auto skip(this BufferReader& self, usize skip_by) -> void { self.offset += skip_by; }

  std::span<const u8> buffer;
  usize offset;
};
} // namespace ox
