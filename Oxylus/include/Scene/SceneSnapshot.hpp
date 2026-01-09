#pragma once

#include <ankerl/unordered_dense.h>
#include <flecs.h>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
struct ComponentState {
  flecs::id_t id = 0;
  u64 hash = 0; // u64_max indicates that this one is a tag
  // TODO: we should probably replace this vector with something pre-allocated
  std::vector<u8> buffer = {};
};

struct EntityState {
  flecs::entity_t entity_id = 0;
  ankerl::unordered_dense::map<flecs::id_t, ComponentState> components = {};
  ankerl::unordered_dense::set<flecs::id_t> removed_components = {};
};

struct SceneState {
  ankerl::unordered_dense::map<flecs::entity_t, EntityState> entities = {};
  ankerl::unordered_dense::set<flecs::entity_t> removed_entities = {};

  auto clear() -> void {
    ZoneScoped;

    entities.clear();
    removed_entities.clear();
  }
};

struct SceneSnapshotBuilder {
  constexpr static auto MAX_SEQUENCES = 32_u32;
  std::array<SceneState, MAX_SEQUENCES> states = {};
  std::array<bool, MAX_SEQUENCES> acks = {};
  u32 current_sequence = 0;

  auto current() -> SceneState& { return states[current_sequence]; }
  auto ack(u32 seq) -> void { acks[seq % MAX_SEQUENCES] = true; }
  auto advance(this SceneSnapshotBuilder&) -> void;
  auto find_last_acked(this SceneSnapshotBuilder& self) -> option<u8>;
  auto delta(this SceneSnapshotBuilder& self) -> SceneState;
  auto take_snapshot(this SceneSnapshotBuilder&, flecs::world& world, SceneState& state) -> void;
};
} // namespace ox
