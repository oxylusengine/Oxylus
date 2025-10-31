#pragma once

#include <string_view>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "OS/OS.hpp"

namespace ox {
struct File {
  option<FileDescriptor> handle;
  usize size = 0;
  FileError error = FileError::None;

  File() = default;
  File(const std::filesystem::path& path, FileAccess access) noexcept;
  File(const File&) = default;
  File(File&&) = default;
  ~File() { close(); }

  auto write_data(const void* data, usize data_size) -> u64;

  template <std::contiguous_iterator Iter>
    requires std::is_trivially_copyable_v<std::iter_value_t<Iter>>
  auto write(Iter first, Iter last) -> u64 {
    using value_type = std::iter_value_t<Iter>;
    auto element_count = std::distance(first, last);
    auto data_size = element_count * sizeof(value_type);

    return write_data(std::to_address(first), data_size);
  }

  template <std::ranges::contiguous_range Range>
    requires std::is_trivially_copyable_v<std::ranges::range_value_t<Range>>
  auto write(Range&& range) -> u64 {
    using value_type = std::ranges::range_value_t<Range>;
    auto data_size = std::ranges::size(range) * sizeof(value_type);

    return write_data(std::ranges::data(range), data_size);
  }

  auto read(void* data, usize data_size) -> u64;
  auto seek(i64 offset) -> void;
  auto close() -> void;

  static auto to_bytes(const std::filesystem::path& path) -> std::vector<u8>;
  static auto to_string(const std::filesystem::path& path) -> std::string;
  static auto to_stdout(std::string_view str) -> void;
  static auto to_stderr(std::string_view str) -> void;

  File& operator=(File&& rhs) noexcept {
    this->handle = rhs.handle;
    this->size = rhs.size;
    this->error = rhs.error;

    rhs.handle.reset();

    return *this;
  }
  bool operator==(const File&) const = default;
  explicit operator bool() { return error == FileError::None; }
};
} // namespace ox
