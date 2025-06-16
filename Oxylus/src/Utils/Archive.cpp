#include "Archive.hpp"

#include "Core/FileSystem.hpp"

namespace ox {
static constexpr uint64_t ARCHIVE_VERSION = 0;

Archive::Archive() { create_empty(); }

Archive::Archive(const std::string& file_name_, bool read_mode_) : read_mode(read_mode_), file_name(file_name_) {
  if (!file_name.empty()) {
    directory = fs::get_directory(file_name);
    if (read_mode) {
      if (const auto content = fs::read_file_binary(file_name); !content.empty()) {
        data_ptr = content.data();
        (*this) >> version;
      }
    } else {
      create_empty();
    }
  }
}

Archive::Archive(const u8* data) {
  data_ptr = data;
  set_read_mode_and_reset_pos(true);
}

void Archive::write_data(std::vector<u8>& dest) const {
  dest.resize(pos);
  std::memcpy(dest.data(), data_ptr, pos);
}

void Archive::create_empty() {
  version = ARCHIVE_VERSION;
  _data.resize(128); // starting size
  data_ptr = _data.data();
  set_read_mode_and_reset_pos(false);
}

void Archive::set_read_mode_and_reset_pos(bool isReadMode) {
  read_mode = isReadMode;
  pos = 0;

  if (read_mode) {
    (*this) >> version;
  } else {
    (*this) << version;
  }
}

void Archive::close() {
  if (!read_mode && !file_name.empty()) {
    fs::write_file_binary(file_name, _data);
  }
  _data.clear();
}

bool Archive::save_file(const std::string_view file_path) const { return fs::write_file_binary(file_path, _data); }

bool Archive::save_header_file(const std::string_view file_path, const std::string_view data_name) const {
  return fs::binary_to_header(file_path, data_name, _data);
}

const std::string& Archive::get_source_directory() const { return directory; }

const std::string& Archive::get_source_file_name() const { return file_name; }

size_t Archive::write_unknown_jump_position() {
  size_t pos_prev = pos;
  _write(uint64_t(pos));
  return pos_prev;
}

void Archive::patch_unknown_jump_position(size_t offset) {
  OX_ASSERT(!read_mode);
  OX_ASSERT(!_data.empty());
  OX_ASSERT(offset + sizeof(uint64_t) < _data.size());
  *(uint64_t*)(_data.data() + offset) = uint64_t(pos);
}
} // namespace ox
