#pragma once

#include <flecs.h>
#include <string_view>
#include <variant>

#include "Core/Types.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox {
struct IEntitySerializer {
  flecs::world& world;

  using Primitive = std::variant<bool*, c8*, i8*, u8*, i16*, u16*, i32*, u32*, i64*, u64*, f32*, f64*>;
  IEntitySerializer(flecs::world& world_) : world(world_) {}
  auto serialize(flecs::entity type, void* component) -> void;
  auto serialize_ops(flecs::meta::op_t* ops, i32 op_count, void* base) -> void;

  virtual auto on_primitive(std::string_view name, Primitive primitive) -> void {};
  virtual auto on_string(std::string_view name, const c8** str) -> void {};
  virtual auto on_entity(std::string_view name, flecs::entity* entity) -> void {};
  virtual auto on_component(std::string_view name, flecs::id_t* component) -> void {};
  virtual auto on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void {};
  virtual auto on_opaque_value(
    std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
  ) -> void {};
};

struct JsonEntitySerializer : IEntitySerializer {
  JsonWriter& writer;

  JsonEntitySerializer(flecs::world& world_, JsonWriter& writer_);

  auto on_primitive(std::string_view name, Primitive primitive) -> void override;
  auto on_string(std::string_view name, const c8** str) -> void override;
  auto on_entity(std::string_view name, flecs::entity* entity) -> void override;
  auto on_component(std::string_view name, flecs::id_t* component) -> void override;
  auto on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void override;
  auto on_opaque_value(
    std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
  ) -> void override;
};

} // namespace ox
