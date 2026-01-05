#include "Scene/EntitySerializer.hpp"

#include <flecs/addons/meta.h>

namespace ox {
auto IEntitySerializer::serialize(flecs::entity type, void* component) -> void {
  ZoneScoped;

  if (type.has<flecs::TypeSerializer>()) {
    const auto& ts = type.get<flecs::TypeSerializer>();
    auto* ops = ecs_vec_first_t(&ts.ops, flecs::meta::op_t);
    auto op_count = ecs_vec_count(&ts.ops);
    serialize_ops(ops, op_count, component);
  }
}

auto IEntitySerializer::serialize_ops(flecs::meta::op_t* ops, i32 op_count, void* base) -> void {
  ZoneScoped;

  for (auto i = 0_i32; i < op_count; i++) {
    const auto& op = ops[i];
    auto name = std::string_view{};
    if (op.name) {
      name = op.name;
    }

    auto* ptr = ECS_OFFSET(base, op.offset);
    switch (op.kind) {
      case EcsOpBool: {
        on_primitive(name, static_cast<bool*>(ptr));
      } break;
      case EcsOpChar: {
        on_primitive(name, static_cast<c8*>(ptr));
      } break;
      case EcsOpU8:
      case EcsOpByte: {
        on_primitive(name, static_cast<u8*>(ptr));
      } break;
      case EcsOpU16: {
        on_primitive(name, static_cast<u16*>(ptr));
      } break;
      case EcsOpU32: {
        on_primitive(name, static_cast<u32*>(ptr));
      } break;
      case EcsOpUPtr:
      case EcsOpU64 : {
        on_primitive(name, static_cast<u64*>(ptr));
      } break;
      case EcsOpI8: {
        on_primitive(name, static_cast<i8*>(ptr));
      } break;
      case EcsOpI16: {
        on_primitive(name, static_cast<i16*>(ptr));
      } break;
      case EcsOpI32: {
        on_primitive(name, static_cast<i32*>(ptr));
      } break;
      case EcsOpIPtr:
      case EcsOpI64 : {
        on_primitive(name, static_cast<i64*>(ptr));
      } break;
      case EcsOpF32: {
        on_primitive(name, static_cast<f32*>(ptr));
      } break;
      case EcsOpF64: {
        on_primitive(name, static_cast<f64*>(ptr));
      } break;

      case EcsOpEntity: {
        on_entity(name, static_cast<flecs::entity*>(ptr));
      } break;

      case EcsOpForward: {
        serialize(flecs::entity(world, op.type), ptr);
      } break;

      case EcsOpId: {
        on_component(name, static_cast<flecs::id_t*>(ptr));
      } break;

      case EcsOpString: {
        on_string(name, static_cast<const c8**>(ptr));
      } break;

      case EcsOpPushStruct: {
        on_struct(name, ops, op.op_count - 1, ptr);
      } break;

      case EcsOpOpaqueValue: {
        on_opaque(name, flecs::entity(world, op.type), ptr);
      } break;

      // these cannot be serialized
      case EcsOpPop:
      case EcsOpScope:
      case EcsOpPrimitive: {
      } break;

      case EcsOpEnum:
      case EcsOpBitmask:
      case EcsOpPushArray:
      case EcsOpPushVector:
      case EcsOpOpaqueStruct:
      case EcsOpOpaqueArray:
      case EcsOpOpaqueVector: {
      } break;
    }

    i += op.op_count - 1;
  }
}

JsonEntitySerializer::JsonEntitySerializer(flecs::world& world_, JsonWriter& writer_)
    : IEntitySerializer(world_),
      writer(writer_) {}

auto JsonEntitySerializer::on_primitive(std::string_view name, Primitive primitive) -> void {
  ZoneScoped;

  auto& element = writer.key(name);
  std::visit(
    ox::match{
      [](const auto&) {},
      [&](bool* v) { element = *v; },
      [&](c8* v) { element = *v; },
      [&](i8* v) { element = *v; },
      [&](u8* v) { element = *v; },
      [&](i16* v) { element = *v; },
      [&](u16* v) { element = *v; },
      [&](i32* v) { element = *v; },
      [&](u32* v) { element = *v; },
      [&](i64* v) { element = *v; },
      [&](u64* v) { element = *v; },
      [&](f32* v) { element = *v; },
      [&](f64* v) { element = *v; },
    },
    primitive
  );
}

auto JsonEntitySerializer::on_string(std::string_view name, const c8** str) -> void { ZoneScoped; }

auto JsonEntitySerializer::on_entity(std::string_view name, flecs::entity* entity) -> void { ZoneScoped; }

auto JsonEntitySerializer::on_component(std::string_view name, flecs::id_t* component) -> void { ZoneScoped; }

auto JsonEntitySerializer::on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void {
  ZoneScoped;
}

auto JsonEntitySerializer::on_opaque(std::string_view name, flecs::entity type, void* ptr) -> void { ZoneScoped; }

} // namespace ox
