#include "Scene/SceneSnapshot.hpp"

#include <fmt/base.h>

#include "Scene/Components.hpp"

namespace ox {
auto SceneSnapshotBuilder::advance(this SceneSnapshotBuilder& self) -> void {
  ZoneScoped;

  self.current_sequence = (self.current_sequence + 1) % MAX_SEQUENCES;
  self.states[self.current_sequence].clear();
  self.acks[self.current_sequence] = false;
}

auto SceneSnapshotBuilder::find_last_acked(this SceneSnapshotBuilder& self) -> option<u8> {
  ZoneScoped;

  for (auto i = 1_u32; i < MAX_SEQUENCES; i++) {
    auto seq = (self.current_sequence + MAX_SEQUENCES - 1) % MAX_SEQUENCES;
    if (self.acks[seq]) {
      return seq;
    }
  }

  return nullopt;
}

auto SceneSnapshotBuilder::delta(this SceneSnapshotBuilder& self) -> SceneState {
  ZoneScoped;

  auto delta = SceneState{};
  const auto& current_state = self.states[self.current_sequence];
  auto last_acked = self.find_last_acked();
  if (!last_acked.has_value()) {
    return current_state;
  }

  const auto& last_state = self.states[last_acked.value()];
  // check for changed entities
  for (const auto& [entity_id, entity_state] : current_state.entities) {
    auto last_it = last_state.entities.find(entity_id);
    if (last_it != last_state.entities.end()) {
      // this entity exist beteen current sequence and last sequence
      const auto& [last_entity_id, last_entity_state] = *last_it;
      auto delta_entity = EntityState{.entity_id = entity_id};
      auto changed = false;
      // check for changed components
      for (const auto& [component_id, component_state] : entity_state.components) {
        auto prev_component_it = last_entity_state.components.find(component_id);
        if (prev_component_it == last_entity_state.components.end() ||
            prev_component_it->second.hash != component_state.hash) {
          delta_entity.components.emplace(component_id, component_state);
          changed = true; // we've inserted new/changed component
        }
      }

      // check for removed components
      for (const auto& [component_id, _] : last_entity_state.components) {
        if (!entity_state.components.contains(component_id)) {
          delta_entity.removed_components.emplace(component_id);
          changed = true; // the component has been removed in current seq.
        }
      }

      if (changed) {
        delta.entities.emplace(entity_id, std::move(delta_entity));
      }
    } else {
      // new entity
      delta.entities.emplace(entity_id, entity_state);
    }
  }

  // check for removed entities
  for (const auto& [entity_id, _] : last_state.entities) {
    if (!current_state.entities.contains(entity_id)) {
      delta.removed_entities.insert(entity_id);
    }
  }

  return delta;
}

auto SceneSnapshotBuilder::take_snapshot(this SceneSnapshotBuilder& self, flecs::world& world, SceneState& state)
  -> void {
  ZoneScoped;

  world.query_builder()
    .with<Networked>() //
    .each([&](flecs::entity component) {
      auto component_id = component.raw_id();
      auto is_component = component.has<flecs::Component>();
      auto component_info = flecs::Component{};
      if (is_component) {
        component_info = component.get<flecs::Component>();
      }

      world.query_builder()
        .with(component) //
        .each([&](flecs::entity entity) {
          auto entity_id = entity.id();
          auto component_state = ComponentState{.id = component_id, .hash = ~0_u64};
          if (is_component) {
            auto* component_data = entity.get(component_id);
            component_state.hash = ankerl::unordered_dense::detail::wyhash::hash(component_data, component_info.size);
            component_state.buffer.resize(component_info.size);
            std::memcpy(component_state.buffer.data(), component_data, component_info.size);
          }

          auto& entity_state = state.entities[entity_id];
          entity_state.entity_id = entity_id;
          entity_state.components.emplace(component_id, std::move(component_state));
        });
    });
}

} // namespace ox
