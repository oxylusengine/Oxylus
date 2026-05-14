#include "OS/File.hpp"

#include "Utils/Log.hpp"

namespace ox {

static auto file_error_to_str(FileError error) -> std::string_view {
  ZoneScoped;

  switch (error) {
    case FileError::None             : return "None";
    case FileError::NoAccess         : return "NoAccess";
    case FileError::Exists           : return "Exists";
    case FileError::IsDir            : return "IsDir";
    case FileError::InUse            : return "InUse";
    case FileError::Interrupted      : return "Interrupted";
    case FileError::BadFileDescriptor: return "BadFileDescriptor";
    case FileError::MapFailed        : return "MapFailed";
    case FileError::Unknown          : return "Unknown";
  }
}

static auto file_access_to_str(FileAccess access) -> std::string_view {
  ZoneScoped;

  switch (access) {
    case FileAccess::Read     : return "Read";
    case FileAccess::Write    : return "Write";
    case FileAccess::ReadWrite: return "ReadWrite";
  }
}

File::File(const std::filesystem::path& path, FileAccess access) : file_path(path) {
  auto file_handle = os::file_open(path, access);
  if (!file_handle.has_value()) {
    this->error = file_handle.error();
    OX_LOG_ERROR(
      "File error: {}, Path: {}, Access: {}",
      file_error_to_str(this->error),
      path.string(),
      file_access_to_str(access)
    );
    return;
  }

  this->handle = file_handle.value();
  this->size = os::file_size(this->handle.value()).value_or(0);
}

auto File::write_data(const void* data, usize data_size) -> u64 {
  ZoneScoped;

  if (!this->handle.has_value()) {
    OX_LOG_ERROR(
      "Couldn't write data into file! Error: {}, Path: {}",
      file_error_to_str(this->error),
      this->file_path.string()
    );
    return {};
  }

  return os::file_write(this->handle.value(), data, data_size);
}

auto File::map() -> void* {
  ZoneScoped;

  auto result = os::file_map(this->handle.value(), this->size);
  if (result.has_value()) {
    this->mapped_data = result.value();
    return result.value();
  }

  this->error = result.error();
  return nullptr;
}

auto File::read(void* data, usize data_size) -> u64 {
  ZoneScoped;

  if (!this->handle.has_value()) {
    OX_LOG_ERROR(
      "Couldn't read into file! Error: {}, Path: {}",
      file_error_to_str(this->error),
      this->file_path.string()
    );
    return {};
  }

  return os::file_read(this->handle.value(), data, data_size);
}

auto File::seek(i64 offset) -> void {
  ZoneScoped;

  if (!this->handle.has_value()) {
    OX_LOG_ERROR(
      "Couldn't seek into file! Error: {}, Path: {}",
      file_error_to_str(this->error),
      this->file_path.string()
    );
    return;
  }

  os::file_seek(this->handle.value(), offset);
}

auto File::close() -> void {
  ZoneScoped;

  if (this->mapped_data.has_value()) {
    os::file_unmap(this->handle.value(), this->mapped_data.value(), this->size);
    this->mapped_data.reset();
  }

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
