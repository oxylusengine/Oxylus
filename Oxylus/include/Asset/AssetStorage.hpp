#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "Core/Types.hpp"
#include "Memory/SlotMap.hpp"
#include "Utils/Log.hpp"

namespace ox {
// immutable payloads in slots that never move, read without a lock. A slot keeps its index for its
// whole life, which is what makes the index usable as a GPU index. Writers replace a payload rather than
// edit it and hand the old one back, for the asset manager to destroy at a commit once no reader of the
// frame can still hold it
template <typename T, SlotMapID ID>
class AssetStorage {
public:
  static constexpr u32 CHUNK_SIZE = 256;
  static constexpr u32 MAX_CHUNKS = 4096;

  AssetStorage() = default;
  AssetStorage(const AssetStorage&) = delete;
  AssetStorage(AssetStorage&&) = delete;
  auto operator=(const AssetStorage&) -> AssetStorage& = delete;
  auto operator=(AssetStorage&&) -> AssetStorage& = delete;
  ~AssetStorage() = default;

  // null for a stale or freed id. Lock-free
  auto get(this const AssetStorage& self, ID id) -> const T* {
    const auto [version, index] = SlotMap_decode_id(id);
    if (index / CHUNK_SIZE >= MAX_CHUNKS) {
      return nullptr;
    }

    const auto* chunk = self.chunks[index / CHUNK_SIZE].load(std::memory_order_acquire);
    if (chunk == nullptr) {
      return nullptr;
    }

    // the payload before the version: a reader that sees a reused slot's new payload also sees the
    // version that was bumped before it, so an old id never resolves to the slot's next asset
    const auto& slot = chunk->slots[index % CHUNK_SIZE];
    const auto* payload = slot.payload.load(std::memory_order_acquire);
    if (slot.version.load(std::memory_order_acquire) != version) {
      return nullptr;
    }

    return payload;
  }

  // owning, for readers that outlive the frame. Takes the writers' lock
  auto share(this AssetStorage& self, ID id) -> std::shared_ptr<const T> {
    auto lock = std::unique_lock(self.mutex);
    auto* slot = self.live_slot(id);
    return slot != nullptr ? slot->owner : nullptr;
  }

  auto create(this AssetStorage& self, std::shared_ptr<const T> payload) -> ID {
    auto lock = std::unique_lock(self.mutex);
    auto index = 0_u32;
    if (!self.free_indices.empty()) {
      index = self.free_indices.back();
      self.free_indices.pop_back();
    } else {
      index = self.next_index++;
      OX_CHECK_LT(index / CHUNK_SIZE, MAX_CHUNKS, "asset storage is full");
      auto& chunk = self.chunks[index / CHUNK_SIZE];
      if (chunk.load(std::memory_order_relaxed) == nullptr) {
        chunk.store(self.owned_chunks.emplace_back(std::make_unique<Chunk>()).get(), std::memory_order_release);
      }
    }

    auto& slot = self.chunks[index / CHUNK_SIZE].load(std::memory_order_relaxed)->slots[index % CHUNK_SIZE];
    slot.owner = std::move(payload);
    slot.payload.store(slot.owner.get(), std::memory_order_release);
    return SlotMap_encode_id<ID>(slot.version.load(std::memory_order_relaxed), index);
  }

  // the slot keeps its id and index. Returns the previous payload, null for a stale id
  auto replace(this AssetStorage& self, ID id, std::shared_ptr<const T> payload) -> std::shared_ptr<const T> {
    auto lock = std::unique_lock(self.mutex);
    auto* slot = self.live_slot(id);
    if (slot == nullptr) {
      return nullptr;
    }

    auto previous = std::exchange(slot->owner, std::move(payload));
    slot->payload.store(slot->owner.get(), std::memory_order_release);
    return previous;
  }

  // returns the payload, null for a stale id
  auto destroy(this AssetStorage& self, ID id) -> std::shared_ptr<const T> {
    auto lock = std::unique_lock(self.mutex);
    auto* slot = self.live_slot(id);
    if (slot == nullptr) {
      return nullptr;
    }

    const auto next_version = slot->version.load(std::memory_order_relaxed) + 1;
    slot->version.store(next_version, std::memory_order_release);
    slot->payload.store(nullptr, std::memory_order_release);
    // a slot whose version would wrap is retired for good rather than risk an old id matching again
    if (next_version != ~0_u32) {
      self.free_indices.push_back(SlotMap_decode_id(id).index);
    }

    return std::move(slot->owner);
  }

  // every slot holding a payload, under the writers' lock
  template <typename Func>
  auto for_each_index(this AssetStorage& self, Func&& func) -> void {
    auto lock = std::unique_lock(self.mutex);
    for (u32 index = 0; index < self.next_index; index++) {
      const auto& slot = self.chunks[index / CHUNK_SIZE].load(std::memory_order_relaxed)->slots[index % CHUNK_SIZE];
      if (slot.owner != nullptr) {
        func(index);
      }
    }
  }

  // destroys every payload in place, for shutdown, when nothing reads any more
  auto destroy_all(this AssetStorage& self) -> void {
    auto lock = std::unique_lock(self.mutex);
    for (auto& chunk : self.owned_chunks) {
      for (auto& slot : chunk->slots) {
        slot.payload.store(nullptr, std::memory_order_release);
        slot.owner.reset();
      }
    }
  }

private:
  struct Slot {
    std::atomic<const T*> payload = nullptr;
    std::atomic<u32> version = 1;
    std::shared_ptr<const T> owner = {};
  };

  struct Chunk {
    std::array<Slot, CHUNK_SIZE> slots = {};
  };

  // under the writers' lock
  auto live_slot(this AssetStorage& self, ID id) -> Slot* {
    const auto [version, index] = SlotMap_decode_id(id);
    if (index / CHUNK_SIZE >= MAX_CHUNKS) {
      return nullptr;
    }

    auto* chunk = self.chunks[index / CHUNK_SIZE].load(std::memory_order_relaxed);
    if (chunk == nullptr) {
      return nullptr;
    }

    auto& slot = chunk->slots[index % CHUNK_SIZE];
    return slot.version.load(std::memory_order_relaxed) == version && slot.owner != nullptr ? &slot : nullptr;
  }

  std::array<std::atomic<Chunk*>, MAX_CHUNKS> chunks = {};
  std::vector<std::unique_ptr<Chunk>> owned_chunks = {};
  std::vector<u32> free_indices = {};
  u32 next_index = 0;
  // writers only, readers never take it
  std::mutex mutex = {};
};
} // namespace ox
