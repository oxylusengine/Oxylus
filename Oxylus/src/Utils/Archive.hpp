#pragma once

namespace ox {
class Archive {
public:
  Archive();
  Archive(const std::string& file_name, bool read_mode = true);
  Archive(const u8* data);
  ~Archive() { close(); }

  void write_data(std::vector<u8>& dest) const;

  const u8* get_data() const { return data_ptr; }
  size_t get_pos() const { return pos; }
  constexpr uint64_t get_version() const { return version; }
  constexpr bool is_read_mode() const { return read_mode; }

  /// @brief This can set the archive into either read or write mode, and it will reset it's position
  void set_read_mode_and_reset_pos(bool isReadMode);

  /// @brief Check if the archive has any data
  bool is_open() const { return data_ptr != nullptr; }

  /// @brief Close the archive. <br>
  ///	If it was opened from a file in write mode, the file will be written at this point <br>
  ///	The data will be deleted, the archive will be empty after this <br>
  void close();

  /// @brief Write the archive contents to a specific file <br>
  ///	The archive data will be written starting from the beginning, to the current position
  bool save_file(std::string_view file_path) const;

  // @brief Write the archive contents into a C++ header file
  /// @param data_name : it will be the name of the byte data array in the header, that can be memory mapped
  /// @param file_path : file name to be saved
  bool save_header_file(std::string_view file_path, std::string_view data_name) const;

  /// @brief If the archive was opened from a file, this will return the file's directory
  const std::string& get_source_directory() const;

  /// @brief If the archive was opened from a file, this will return the file's name <br>
  /// The file's name will include the directory as well
  const std::string& get_source_file_name() const;

  // Appends the current archive write offset as uint64_t to the archive
  //	Returns the previous write offset of the archive, which can be used by PatchUnknownJumpPosition()
  //	to write the current archive position to that previous position
  size_t write_unknown_jump_position();

  // Writes the current archive write offset to the specified archive write offset.
  //	It can be used with Jump() to skip parts of the archive when reading
  void patch_unknown_jump_position(size_t offset);

  // Modifies the current archive offset
  //	It can be used in conjunction with WriteUnknownJumpPosition() and PatchUnknownJumpPosition()
  void jump(uint64_t jump_pos) { pos = jump_pos; }

  // It could be templated but we have to be extremely careful of different datasizes on different platforms
  // because serialized data should be interchangeable!
  // So providing exact copy operations for exact types enforces platform agnosticism

  // Write operations
  Archive& operator<<(bool data) {
    _write((uint32_t)(data ? 1 : 0));
    return *this;
  }
  Archive& operator<<(char data) {
    _write((int8_t)data);
    return *this;
  }
  Archive& operator<<(unsigned char data) {
    _write((u8)data);
    return *this;
  }
  Archive& operator<<(int data) {
    _write((int64_t)data);
    return *this;
  }
  Archive& operator<<(unsigned int data) {
    _write((uint64_t)data);
    return *this;
  }
  Archive& operator<<(long data) {
    _write((int64_t)data);
    return *this;
  }
  Archive& operator<<(unsigned long data) {
    _write((uint64_t)data);
    return *this;
  }
  Archive& operator<<(long long data) {
    _write((int64_t)data);
    return *this;
  }
  Archive& operator<<(unsigned long long data) {
    _write((uint64_t)data);
    return *this;
  }
  Archive& operator<<(float data) {
    _write(data);
    return *this;
  }
  Archive& operator<<(double data) {
    _write(data);
    return *this;
  }
  Archive& operator<<(const std::string& data) {
    (*this) << data.length();
    for (const auto& x : data) {
      (*this) << x;
    }
    return *this;
  }
  template <typename T>
  Archive& operator<<(const std::vector<T>& data) {
    // Here we will use the << operator so that non-specified types will have compile error!
    (*this) << data.size();
    for (const T& x : data) {
      (*this) << x;
    }
    return *this;
  }

  // TODO: Write operations for Vectors etc.

  // Read operations
  Archive& operator>>(bool& data) {
    uint32_t temp;
    _read(temp);
    data = (temp == 1);
    return *this;
  }
  Archive& operator>>(char& data) {
    int8_t temp;
    _read(temp);
    data = (char)temp;
    return *this;
  }
  Archive& operator>>(unsigned char& data) {
    u8 temp;
    _read(temp);
    data = (unsigned char)temp;
    return *this;
  }
  Archive& operator>>(int& data) {
    int64_t temp;
    _read(temp);
    data = (int)temp;
    return *this;
  }
  Archive& operator>>(unsigned int& data) {
    uint64_t temp;
    _read(temp);
    data = (unsigned int)temp;
    return *this;
  }
  Archive& operator>>(long& data) {
    int64_t temp;
    _read(temp);
    data = (long)temp;
    return *this;
  }
  Archive& operator>>(unsigned long& data) {
    uint64_t temp;
    _read(temp);
    data = (unsigned long)temp;
    return *this;
  }
  Archive& operator>>(long long& data) {
    int64_t temp;
    _read(temp);
    data = (long long)temp;
    return *this;
  }
  Archive& operator>>(unsigned long long& data) {
    uint64_t temp;
    _read(temp);
    data = (unsigned long long)temp;
    return *this;
  }
  Archive& operator>>(float& data) {
    _read(data);
    return *this;
  }
  Archive& operator>>(double& data) {
    _read(data);
    return *this;
  }

  Archive& operator>>(std::string& data) {
    uint64_t len;
    (*this) >> len;
    data.resize(len);
    for (size_t i = 0; i < len; ++i) {
      (*this) >> data[i];
    }
    return *this;
  }

  // TODO: Read operations for Vectors etc.

private:
  uint32_t version = 0;
  bool read_mode = false;
  size_t pos = 0;
  std::vector<u8> _data = {};
  const u8* data_ptr = nullptr;

  std::string file_name = {}; // save to this file on close
  std::string directory = {}; // directory of file_name

  void create_empty();        // create new archive in write mode

  // This should not be exposed to avoid misaligning data by mistake
  // Any specific type serialization should be implemented by hand
  // But these can be used as helper functions inside this class

  template <typename T>
  void _write(const T& data) {
    OX_ASSERT(!read_mode);
    OX_ASSERT(!_data.empty());
    const size_t _right = pos + sizeof(data);
    if (_right > _data.size()) {
      _data.resize(_right * 2);
      data_ptr = _data.data();
    }
    *(T*)(_data.data() + pos) = data;
    pos = _right;
  }

  template <typename T>
  void _read(T& data) {
    OX_ASSERT(read_mode);
    OX_ASSERT(data_ptr != nullptr);
    data = *(const T*)(data_ptr + pos);
    pos += (size_t)(sizeof(data));
  }
};
} // namespace ox
