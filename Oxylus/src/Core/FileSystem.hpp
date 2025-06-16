#pragma once

namespace ox {
/// @brief A lightweight std::filesystem alternative and a safe wrapper for std::filesystem
namespace fs {
/// @return current working dir
std::string current_path();
/// @return directory and file
std::pair<std::string, std::string> split_path(std::string_view full_path);
/// @return What's left after the last dot in `filepath`
std::string get_file_extension(std::string_view filepath);
/// @return File name without the extension
std::string get_file_name(std::string_view filepath);
/// @return File name with the extension
std::string get_name_with_extension(std::string_view filepath);
/// @return Directory from the given path
std::string get_directory(std::string_view path);
/// @brief append a pair of paths together
std::string append_paths(std::string_view path, std::string_view second_path);
/// @brief convert paths with '\\' into '/'
std::string preferred_path(std::string_view path);
/// @brief open and select the file in OS
void open_folder_select_file(std::string_view path);
/// @brief open the file in an external program
void open_file_externally(std::string_view path);
/// @brief copy file
void copy_file(std::string_view from, std::string_view to);
/// @brief remove dir/file
void remove(std::string_view path);
/// @brief dir/file exists
bool exists(std::string_view path);
/// @brief absolute physical path
std::string absolute(std::string_view path);
/// @brief returns last directory/file from the path without slashes
std::string get_last_component(std::string_view path);

std::string read_file(std::string_view file_path);
std::vector<u8> read_file_binary(std::string_view file_path);

template <typename T>
bool write_file(const std::string_view file_path, const T& data, const std::string& comment = {}) {
  std::stringstream ss;
  ss << comment << "\n" << data;
  std::ofstream filestream(file_path.data());
  if (filestream.is_open()) {
    filestream << ss.str();
    return true;
  }

  return false;
}

bool write_file_binary(std::string_view file_path, const std::vector<u8>& data);

bool binary_to_header(std::string_view file_path, std::string_view data_name, const std::vector<u8>& data);
}; // namespace fs
} // namespace ox
