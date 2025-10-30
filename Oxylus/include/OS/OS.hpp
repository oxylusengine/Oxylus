#pragma once

#include <expected>
#include <filesystem>
#include <string_view>
#include <thread>

#include "Core/Enum.hpp" // IWYU pragma: export
#include "Core/Types.hpp"

namespace ox {
enum class FileError : i32 {
  None = 0,
  NoAccess,
  Exists,
  IsDir,
  InUse,
  Interrupted,
  BadFileDescriptor,
  Unknown,
};
constexpr bool operator!(FileError v) { return v != FileError::None; }

enum class FileAccess {
  Read,
  Write,
  ReadWrite
};

// Can we add stdout and other pipes here?
enum class FileDescriptor : uptr { Invalid = 0 };

namespace os {
// Memory
auto mem_page_size() -> u64;
auto mem_reserve(u64 size) -> void*;
auto mem_release(void* data, u64 size = 0) -> void;
auto mem_commit(void* data, u64 size) -> bool;
auto mem_decommit(void* data, u64 size) -> void;

// Threads
auto thread_id() -> i64;
auto set_thread_name(std::string_view name) -> void;
auto set_thread_name(std::thread::native_handle_type thread, std::string_view name) -> void;

// IO
auto open_folder_select_file(const std::filesystem::path& path) -> void;
auto open_file_externally(const std::filesystem::path& path) -> void;
auto file_open(const std::filesystem::path& path, FileAccess access) -> std::expected<FileDescriptor, FileError>;
auto file_close(FileDescriptor file) -> void;
auto file_size(FileDescriptor file) -> std::expected<usize, FileError>;
auto file_read(FileDescriptor file, void* data, usize size) -> usize;
auto file_write(FileDescriptor file, const void* data, usize size) -> usize;
auto file_seek(FileDescriptor file, i64 offset) -> void;
auto file_stdout(std::string_view str) -> void;
auto file_stderr(std::string_view str) -> void;
} // namespace os
} // namespace ox
