#include "OS/File.hpp"

namespace ox {
File::File(const std::filesystem::path& path, FileAccess access) {
  auto file_handle = os::file_open(path, access);
  if (!file_handle.has_value()) {
    this->error = file_handle.error();
    return;
  }

  this->handle = file_handle.value();
  this->size = os::file_size(this->handle.value()).value_or(0);
}

auto File::write_data(const void* data, usize data_size) -> u64 {
  ZoneScoped;

  return os::file_write(this->handle.value(), data, data_size);
}

auto File::read(void* data, usize data_size) -> u64 {
  ZoneScoped;

  return os::file_read(this->handle.value(), data, data_size);
}

auto File::seek(i64 offset) -> void {
  ZoneScoped;

  os::file_seek(this->handle.value(), offset);
}

auto File::close() -> void {
  ZoneScoped;

  if (this->handle.has_value()) {
    os::file_close(this->handle.value());
    this->handle.reset();
  }
}

auto File::to_bytes(const std::filesystem::path& path) -> std::vector<u8> {
  ZoneScoped;

  File file(path, FileAccess::Read);
  if (!file) {
    return {};
  }

  auto contents = std::vector<u8>();
  // intentionally reserve+resize to insert null termination
  contents.reserve(file.size + 1);
  contents.resize(file.size);

  file.read(contents.data(), file.size);

  return contents;
}

auto File::to_string(const std::filesystem::path& path) -> std::string {
  ZoneScoped;

  File file(path, FileAccess::Read);
  if (!file) {
    return {};
  }

  std::string str;
  str.resize(file.size);
  file.read(str.data(), file.size);

  return str;
}

auto File::to_stdout(std::string_view str) -> void {
  ZoneScoped;

  os::file_stdout(str);
}

auto File::to_stderr(std::string_view str) -> void {
  ZoneScoped;

  os::file_stderr(str);
}

} // namespace ox
