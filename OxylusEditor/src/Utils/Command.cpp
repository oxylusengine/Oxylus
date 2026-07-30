#include "Utils/Command.hpp"

#include <simdjson.h>

namespace ox {
auto EntityDeleteCommand::undo() -> void {
  auto content = simdjson::padded_string(serialized_entity_);
  simdjson::ondemand::parser parser;
  auto doc = parser.iterate(content);
  auto entities_array = doc["entities"];
  std::vector<UUID> requested_assets = {};
  for (auto entity_json : entities_array.get_array()) {
    entity_ = Scene::json_to_entity(
      *scene_, //
      flecs::entity::null(),
      entity_json.value_unsafe(),
      requested_assets
    );
  }
}
} // namespace ox
