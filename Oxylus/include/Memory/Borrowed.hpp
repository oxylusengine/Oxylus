#pragma once

#include <shared_mutex>

namespace ox {
template <typename T>
struct Borrowed {
  T* value = nullptr;
  std::shared_mutex* mutex = nullptr;

  Borrowed(const Borrowed&) = delete;
  Borrowed& operator=(const Borrowed&) = delete;

  Borrowed() noexcept : value(nullptr), mutex(nullptr) {}
  Borrowed(std::shared_mutex& mutex_, T* value_) noexcept : value(value_), mutex(&mutex_) { mutex->lock_shared(); }
  Borrowed(Borrowed&& other) noexcept : value(other.value), mutex(other.mutex) {
    other.value = nullptr;
    other.mutex = nullptr;
  }
  ~Borrowed() noexcept { reset(); }

  Borrowed& operator=(Borrowed&& other) noexcept {
    if (mutex) {
      mutex->unlock_shared();
    }

    value = other.value;
    mutex = other.mutex;
    other.value = nullptr;
    other.mutex = nullptr;

    return *this;
  }

  T* operator->() const noexcept { return value; }
  explicit operator bool() const noexcept { return value != nullptr; }

  auto reset() -> void {
    if (mutex) {
      mutex->unlock_shared();
      mutex = nullptr;
    }

    value = nullptr;
  }
};
} // namespace ox
