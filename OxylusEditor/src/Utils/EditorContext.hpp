#pragma once
#include <cstring>
#include <flecs.h>

namespace ox {
struct EditorContext {
  enum class Type { None = 0, Entity, File };

  Type type = Type::None;
  option<std::string> str = ox::nullopt;
  option<flecs::entity> entity = ox::nullopt;

  auto reset(
    this EditorContext& self,
    Type type = Type::None,
    option<std::string> str = nullopt,
    option<flecs::entity> entity = nullopt
  ) -> void {
    self.type = type;
    self.str = str;
    self.entity = entity;
  }
};
} // namespace ox
