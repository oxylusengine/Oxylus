#pragma once

#include <atomic>
#include <type_traits>
#include <utility>

#include "Core/Types.hpp"

namespace ox {
struct ManagedObj {
private:
  constexpr static auto INVALID_REF = ~0_u64;
  u64 ref_count = 0;

protected:
  constexpr ~ManagedObj() = default;

public:
  constexpr ManagedObj() = default;
  ManagedObj(ManagedObj&&) = delete;
  ManagedObj(const ManagedObj&) = delete;
  ManagedObj& operator=(ManagedObj&&) = delete;
  ManagedObj& operator=(const ManagedObj&) = delete;

  auto acquire_ref(this ManagedObj& self) -> void {
    ZoneScoped;

    ++std::atomic_ref(self.ref_count);
  }

  auto release_ref(this ManagedObj& self) -> bool {
    ZoneScoped;

    auto free_this = !--std::atomic_ref(self.ref_count);
    if (free_this) {
      std::atomic_ref(self.ref_count).store(INVALID_REF);
    }

    return free_this;
  }
};

template <typename T>
struct Arc {
private:
  T* ptr = nullptr;

public:
  template <typename... Args>
  static auto create(Args&&... args) -> Arc<T> {
    ZoneScoped;

    return Arc(new T(std::forward<Args>(args)...));
  }

  constexpr Arc() = default;
  constexpr Arc(T* ptr_);
  constexpr Arc(std::nullptr_t);
  constexpr ~Arc();

  constexpr Arc(const Arc&);
  constexpr Arc(Arc&&) noexcept;
  constexpr auto operator=(const Arc&) -> Arc&;
  constexpr auto operator=(Arc&&) noexcept -> Arc&;

  [[nodiscard]]
  constexpr operator bool() const;
  [[nodiscard]]
  constexpr auto operator!() const -> bool;
  [[nodiscard]]
  constexpr auto operator*() const -> T&;
  [[nodiscard]]
  constexpr auto operator->() const -> T*;
  [[nodiscard]]
  constexpr auto operator->() -> T*;
  template <typename U>
  [[nodiscard]]
  constexpr auto operator<=>(const Arc<U>& other) const -> std::strong_ordering;

  template <typename U>
  [[nodiscard]]
  constexpr auto as() const -> Arc<U>;
  template <typename U = std::add_const_t<T>>
    requires(!std::is_const_v<T>)
  [[nodiscard]]
  constexpr auto as() const -> Arc<U>;
  [[nodiscard]]
  constexpr auto get() const -> T*;
  constexpr auto release() -> T*;
  constexpr auto reset() -> void;

  template <typename U>
  friend struct Arc;
};

//   ──────────────────────────────────────────────────────────────────────

template <typename T>
Arc(T*) -> Arc<T>;

template <typename T>
Arc(const T*) -> Arc<const T>;

//   ──────────────────────────────────────────────────────────────────────

template <typename T>
constexpr Arc<T>::Arc(T* ptr_) : ptr(ptr_) {
  ZoneScoped;

  if (ptr_) {
    ptr_->acquire_ref();
  }
}

template <typename T>
constexpr Arc<T>::Arc(std::nullptr_t) : ptr(nullptr) {
  ZoneScoped;
}

template <typename T>
constexpr Arc<T>::~Arc() {
  ZoneScoped;

  this->reset();
}

template <typename T>
constexpr Arc<T>::Arc(const Arc& other_) : Arc(other_.ptr) {
  ZoneScoped;
}

template <typename T>
constexpr Arc<T>::Arc(Arc&& other_) noexcept : ptr(std::exchange(other_.ptr, nullptr)) {
  ZoneScoped;
}

template <typename T>
constexpr auto Arc<T>::operator=(const Arc& other) -> Arc& {
  ZoneScoped;

  if (this != &other) {
    this->~Arc();
    new (this) Arc(other);
  }

  return *this;
}

template <typename T>
constexpr auto Arc<T>::operator=(Arc&& other) noexcept -> Arc& {
  ZoneScoped;

  if (this != &other) {
    this->~Arc();
    new (this) Arc(std::move(other));
  }

  return *this;
}

template <typename T>
constexpr Arc<T>::operator bool() const {
  return ptr;
}

template <typename T>
constexpr auto Arc<T>::operator!() const -> bool {
  return !ptr;
}

template <typename T>
constexpr auto Arc<T>::operator*() const -> T& {
  return *ptr;
}

template <typename T>
constexpr auto Arc<T>::operator->() const -> T* {
  return ptr;
}

template <typename T>
constexpr auto Arc<T>::operator->() -> T* {
  return ptr;
}

template <typename T>
template <typename U>
constexpr auto Arc<T>::operator<=>(const Arc<U>& other) const -> std::strong_ordering {
  return ptr <=> other.ptr;
}

template <typename T>
template <typename U>
constexpr auto Arc<T>::as() const -> Arc<U> {
  return static_cast<Arc<U>>(ptr);
}

template <typename T>
template <typename U>
  requires(!std::is_const_v<T>)
constexpr auto Arc<T>::as() const -> Arc<U> {
  return static_cast<Arc<U>>(ptr);
}

template <typename T>
constexpr auto Arc<T>::get() const -> T* {
  return ptr;
}

template <typename T>
constexpr auto Arc<T>::release() -> T* {
  ZoneScoped;

  if (!ptr) {
    return nullptr;
  }

  OX_CHECK_EQ(ptr->release_ref(), true, "Releasing Arc pointer still has references.");
  return std::exchange(ptr, nullptr);
}

template <typename T>
constexpr auto Arc<T>::reset() -> void {
  ZoneScoped;

  if (ptr && ptr->release_ref()) {
    delete ptr;
  }

  ptr = nullptr;
}
} // namespace ox
