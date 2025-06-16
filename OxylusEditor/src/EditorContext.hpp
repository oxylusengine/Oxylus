#pragma once
#include <cstring>
#include <flecs.h>

namespace ox {
struct EditorContext {
  enum class Type { None = 0, Entity, File };

  Type type = Type::None;
  option<std::string> str = ox::nullopt;
  option<flecs::entity> entity = ox::nullopt;

  auto reset() -> void {
    type = Type::None;
    str.reset();
    entity.reset();
  }
};
} // namespace ox
