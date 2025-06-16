#pragma once

#include <shared_mutex>
#include <span>

namespace ox {
// For unions and other unsafe stuff
struct SlotMapIDUnpacked {
  u32 version = ~0_u32;
  u32 index = ~0_u32;
};

template <typename T>
concept SlotMapID = std::is_enum_v<T> &&                            // ID must be an enum to preserve strong typing.
                    std::is_same_v<u64, std::underlying_type_t<T>>; // ID enum must have underlying type of u64.

constexpr static u64 SLOT_MAP_VERSION_BITS = 32_u64;
constexpr static u64 SLOT_MAP_INDEX_MASK = (1_u64 << SLOT_MAP_VERSION_BITS) - 1_u64;

template <SlotMapID ID>
constexpr auto SlotMap_encode_id(u32 version, u32 index) -> ID {
  ZoneScoped;

  u64 raw = (static_cast<u64>(version) << SLOT_MAP_VERSION_BITS) | static_cast<u64>(index);
  return static_cast<ID>(raw);
}

template <SlotMapID ID>
constexpr auto SlotMap_decode_id(ID id) -> SlotMapIDUnpacked {
  ZoneScoped;

  auto raw = static_cast<u64>(id);
  auto version = static_cast<u32>(raw >> SLOT_MAP_VERSION_BITS);
  auto index = static_cast<u32>(raw & SLOT_MAP_INDEX_MASK);

  return {.version = version, .index = index};
}

// Modified version of:
//     https://github.com/Sunset-Flock/Timberdoodle/blob/398b6e27442a763668ecf75b6a0c3a29c7a13884/src/slot_map.hpp#L10
template <typename T, SlotMapID ID>
struct SlotMap {
  using Self = SlotMap<T, ID>;

private:
  std::vector<T> slots = {};
  // this is vector of dynamic bitsets. T != char/bool
  std::vector<bool> states = {}; // slot state, useful when iterating
  std::vector<u32> versions = {};

  std::vector<usize> free_indices = {};
  mutable std::shared_mutex mutex = {};

public:
  auto create_slot(this Self& self, T&& v = {}) -> ID {
    ZoneScoped;

    std::unique_lock _(self.mutex);
    if (not self.free_indices.empty()) {
      auto index = self.free_indices.back();
      self.free_indices.pop_back();
      self.slots[index] = std::move(v);
      self.states[index] = true;
      return SlotMap_encode_id<ID>(self.versions[index], static_cast<u32>(index));
    }
    auto index = static_cast<u32>(self.slots.size());
    self.slots.emplace_back(std::move(v));
    self.states.emplace_back(true);
    self.versions.emplace_back(1_u32);
    return SlotMap_encode_id<ID>(1_u32, index);
  }

  auto destroy_slot(this Self& self, ID id) -> bool {
    ZoneScoped;

    if (self.is_valid(id)) {
      std::unique_lock lock(self.mutex);
      auto index = SlotMap_decode_id(id).index;
      self.states[index] = false;
      self.versions[index] += 1;
      if (self.versions[index] < ~0_u32) {
        self.free_indices.push_back(index);
      }

      return true;
    }

    return false;
  }

  auto reset(this Self& self) -> void {
    ZoneScoped;

    std::unique_lock _(self.mutex);
    self.slots.clear();
    self.versions.clear();
    self.states.clear();
    self.free_indices.clear();
  }

  auto is_valid(this const Self& self, ID id) -> bool {
    ZoneScoped;

    std::shared_lock _(self.mutex);
    auto [version, index] = SlotMap_decode_id(id);
    return index < self.slots.size() && self.versions[index] == version;
  }

  auto slot(this Self& self, ID id) -> T* {
    ZoneScoped;

    if (self.is_valid(id)) {
      std::shared_lock _(self.mutex);
      auto index = SlotMap_decode_id(id).index;
      return &self.slots[index];
    }

    return nullptr;
  }

  auto slotc(this const Self& self, ID id) -> const T* {
    ZoneScoped;

    if (self.is_valid(id)) {
      std::shared_lock _(self.mutex);
      auto index = SlotMap_decode_id(id).index;
      return &self.slots[index];
    }

    return nullptr;
  }

  auto slot_from_index(this Self& self, usize index) -> T* {
    ZoneScoped;

    std::shared_lock _(self.mutex);
    if (index < self.slots.size() && self.states[index]) {
      return &self.slots[index];
    }

    return nullptr;
  }

  auto size(this const Self& self) -> usize {
    ZoneScoped;

    std::shared_lock _(self.mutex);
    return self.slots.size() - self.free_indices.size();
  }

  auto capacity(this const Self& self) -> usize {
    ZoneScoped;

    std::shared_lock _(self.mutex);
    return self.slots.size();
  }

  auto slots_unsafe(this Self& self) -> std::span<T> {
    ZoneScoped;

    std::shared_lock _(self.mutex);
    return self.slots;
  }

  auto get_mutex(this Self& self) -> std::shared_mutex& {
    ZoneScoped;

    return self.mutex;
  }
};
} // namespace ox
