#pragma once

#include <concepts>

namespace ox {
// a lock-free read of a published payload. It stays valid until the next `AssetManager::commit`, the
// earliest a replaced or unloaded payload is destroyed, so never keep one across a frame or hand it to a
// job that may outlive the frame
template <typename T>
struct AssetView {
  const T* value = nullptr;

  auto operator->(this const AssetView& self) noexcept -> const T* { return self.value; }
  auto operator*(this const AssetView& self) noexcept -> const T& { return *self.value; }
  explicit operator bool() const noexcept { return value != nullptr; }

  auto reset(this AssetView& self) noexcept -> void { self.value = nullptr; }

  auto copy(this const AssetView& self) -> T
    requires std::copy_constructible<T>
  {
    return *self.value;
  }
};
} // namespace ox
