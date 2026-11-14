#pragma once

#include <atomic>
#include <expected>
#include <string>

namespace ox {
// the result is written before `is_done` is released
template <typename T>
struct AsyncResult {
  std::atomic<bool> is_done = false;
  std::expected<T, std::string> result = std::unexpected(std::string());

  auto finish(this AsyncResult& self, T value) -> void {
    self.result = std::move(value);
    self.is_done.store(true, std::memory_order_release);
  }

  auto fail(this AsyncResult& self, std::string error) -> void {
    self.result = std::unexpected(std::move(error));
    self.is_done.store(true, std::memory_order_release);
  }

  auto is_ready(this const AsyncResult& self) -> bool { return self.is_done.load(std::memory_order_acquire); }
};
} // namespace ox
