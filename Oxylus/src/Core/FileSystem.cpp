#include "FileSystem.hpp"

#include "Utils/StringUtils.hpp"

#ifdef OX_PLATFORM_WINDOWS
  #include <ShlObj_core.h>
  #include <comdef.h>
  #include <shellapi.h>
#endif

namespace ox {
std::string fs::current_path() { return std::filesystem::current_path().generic_string(); }

std::pair<std::string, std::string> fs::split_path(const std::string_view full_path) {
  const size_t found = full_path.find_last_of("/\\");
  return {
      std::string(full_path.substr(0, found + 1)), // directory
      std::string(full_path.substr(found + 1)),    // file
  };
}

std::string fs::get_file_extension(const std::string_view filepath) {
  const auto lastDot = filepath.find_last_of('.');
  return std::string(filepath.substr(lastDot + 1, filepath.size() - lastDot));
}

std::string fs::get_file_name(const std::string_view filepath) {
  auto [dir, file] = split_path(filepath);
  const auto lastDot = file.rfind('.');
  return std::string(file.substr(0, lastDot));
}

std::string fs::get_name_with_extension(const std::string_view filepath) { return split_path(filepath).second; }

std::string fs::get_directory(const std::string_view path) {
  if (path.empty()) {
    return std::string(path);
  }

  return std::filesystem::path(path).parent_path().generic_string();
}

std::string fs::append_paths(const std::string_view path, const std::string_view second_path) {
  if (path.empty() || second_path.empty())
    return std::string(path);

  std::string new_path(path);
  new_path.append("/").append(second_path);
  return preferred_path(new_path);
}

std::string fs::preferred_path(const std::string_view path) {
  return std::filesystem::path(path).make_preferred().generic_string();
}

void fs::open_folder_select_file(const std::string_view path) {
#ifdef OX_PLATFORM_WINDOWS
  std::string p_str(path);
  StringUtils::replace_string(p_str, "/", "\\");
  const _bstr_t widePath(p_str.c_str());
  if (const auto pidl = ILCreateFromPath(widePath)) {
    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); // TODO: check for result
    ILFree(pidl);
  }
#else
  OX_LOG_WARN("Not implemented on this platform!");
#endif
}

void fs::open_file_externally(const std::string_view path) {
#ifdef OX_PLATFORM_WINDOWS
  const _bstr_t widePath(path.data());
  ShellExecute(nullptr, L"open", widePath, nullptr, nullptr, SW_RESTORE);
#else
  OX_LOG_WARN("Not implemented on this platform!");
#endif
}

void fs::copy_file(const std::string_view from, const std::string_view to) {
  try {
    std::filesystem::copy(from, to);
  } catch (std::exception& exception) {
    OX_LOG_ERROR(exception.what());
  }
}

void fs::remove(const std::string_view path) {
  try {
    std::filesystem::remove(path);
  } catch (std::exception& exception) {
    OX_LOG_ERROR(exception.what());
  }
}

bool fs::exists(const std::string_view path) { return std::filesystem::exists(path); }

std::string fs::absolute(const std::string_view path) {
  const auto p = std::filesystem::absolute(path);
  return p.generic_string();
}

std::string fs::get_last_component(const std::string_view path) {
  auto last = std::filesystem::path(path).filename().generic_string();
  return last;
}

std::string fs::read_file(const std::string_view file_path) {
  ZoneScoped;
  std::ostringstream buf;
  const std::ifstream input(file_path.data());
  buf << input.rdbuf();
  return buf.str();
}

std::vector<u8> fs::read_file_binary(const std::string_view file_path) {
  std::ifstream file(file_path.data(), std::ios::binary);

  std::vector<u8> data = {};
  if (file.is_open()) {
    const size_t dataSize = file.tellg();
    file.seekg(0, file.beg);
    data.resize(dataSize);
    file.read((char*)data.data(), dataSize);
  }
  file.close();

  return data;
}

bool fs::write_file_binary(const std::string_view file_path, const std::vector<u8>& data) {
  std::ofstream file(file_path.data(), std::ios::binary | std::ios::trunc);
  if (file.is_open()) {
    file.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    file.close();
    return true;
  }
  return false;
}

bool
fs::binary_to_header(const std::string_view file_path, const std::string_view data_name, const std::vector<u8>& data) {
  std::string ss;
  ss += "const u8 ";
  ss += data_name;
  ss += "[] = {";
  for (size_t i = 0; i < data.size(); ++i) {
    if (i % 32 == 0) {
      ss += "\n";
    }
    ss += std::to_string((uint32_t)data[i]) + ",";
  }
  ss += "\n};\n";
  return write_file(file_path, ss, "// Oxylus generated header file");
}
} // namespace ox
