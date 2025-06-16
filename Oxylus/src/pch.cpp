#if TRACY_ENABLE

static void* ox_aligned_alloc(ox::usize size, ox::usize alignment = alignof(ox::usize)) {
  #if OX_PLATFORM_WINDOWS == 1
  return _aligned_malloc(size, alignment);
  #elif OX_PLATFORM_LINUX == 1
  void* data = nullptr;
  posix_memalign(&data, alignment, size);
  return data;
  #else
    #error "Unknown platform"
  #endif
}

static void ox_aligned_free(void* ptr) {
  #if OX_PLATFORM_WINDOWS == 1
  _aligned_free(ptr);
  #elif OX_PLATFORM_LINUX == 1
  free(ptr);
  #else
    #error "Unknown platform"
  #endif
}

// https://en.cppreference.com/w/cpp/memory/new/operator_new
// Ignore non-allocating operators (for std::construct_at, placement new)

[[nodiscard]]
void* operator new(std::size_t size) {
  auto ptr = ox_aligned_alloc(size);
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete(void* ptr) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

[[nodiscard]]
void* operator new[](std::size_t size) {
  auto ptr = ox_aligned_alloc(size);
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete[](void* ptr) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

[[nodiscard]]
void* operator new(std::size_t size, std::align_val_t alignment) {
  auto ptr = ox_aligned_alloc(size, static_cast<ox::usize>(alignment));
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

[[nodiscard]]
void* operator new[](std::size_t size, std::align_val_t alignment) {
  auto ptr = ox_aligned_alloc(size, static_cast<ox::usize>(alignment));
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

[[nodiscard]]
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  auto ptr = ox_aligned_alloc(size);
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

[[nodiscard]]
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  auto ptr = ox_aligned_alloc(size);
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

[[nodiscard]]
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  auto ptr = ox_aligned_alloc(size, static_cast<ox::usize>(alignment));
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

[[nodiscard]]
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  auto ptr = ox_aligned_alloc(size, static_cast<ox::usize>(alignment));
  TracyAlloc(ptr, size);
  return ptr;
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  TracyFree(ptr);
  ox_aligned_free(ptr);
}

#endif
