#pragma once

#include <concepts>
#include <shared_mutex>

namespace ox {
// Replica of `std::adopt_lock`, to avoid TOCTOU in ReadGuard
struct adopt_lock_t {};
inline constexpr adopt_lock_t adopt_lock{};

template <typename T>
struct ReadGuard {
  T* value = nullptr;
  std::shared_mutex* mutex = nullptr;

  ReadGuard(const ReadGuard&) = delete;
  ReadGuard& operator=(const ReadGuard&) = delete;

  ReadGuard() noexcept : value(nullptr), mutex(nullptr) {}

  // Acquires the shared lock
  ReadGuard(std::shared_mutex& mutex_, T* value_) noexcept : value(value_), mutex(&mutex_) { mutex->lock_shared(); }
  // Adopts an already-held shared lock (caller locked before searching)
  ReadGuard(std::shared_mutex& mutex_, T* value_, adopt_lock_t) noexcept : value(value_), mutex(&mutex_) {}

  ReadGuard(ReadGuard&& other) noexcept : value(other.value), mutex(other.mutex) {
    other.value = nullptr;
    other.mutex = nullptr;
  }
  ~ReadGuard() noexcept { reset(); }

  ReadGuard& operator=(ReadGuard&& other) noexcept {
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

  auto copy() const noexcept(std::is_nothrow_copy_constructible_v<T>) -> T
    requires std::copy_constructible<T>
  {
    return *value;
  }
};
} // namespace ox
