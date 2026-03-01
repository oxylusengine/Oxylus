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

  struct OpaqueContext {
    IEntitySerializer* self = nullptr;
    std::string_view name = {};
    flecs::entity_t field_type = {};
    void* field_ptr = nullptr;
  };

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
        on_struct(name, ops + i, op.op_count - 1, ptr);
      } break;

      case EcsOpOpaqueValue: {
        auto opaque_ctx = OpaqueContext{.self = this, .name = name, .field_type = op.type, .field_ptr = ptr};
        auto serializer = flecs::serializer{};
        serializer.world = world;
        serializer.ctx = &opaque_ctx;
        serializer.value_ = [](const struct ecs_serializer_t* ser, ecs_entity_t type, const void* value) -> i32 {
          const auto& [self, field_name, field_type, field_ptr] = *static_cast<OpaqueContext*>(ser->ctx);
          self->on_opaque_value(field_name, field_type, field_ptr, type, value);
          return 0;
        };
        auto* opaque_value = ecs_get(world, op.type, EcsOpaque);
        opaque_value->serialize(&serializer, ptr);
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

auto JsonEntitySerializer::on_string(std::string_view name, const c8** str) -> void {
  ZoneScoped;

  if (str && *str) {
    writer[name] = *str;
  }
}

auto JsonEntitySerializer::on_entity(std::string_view name, flecs::entity* entity) -> void {
  ZoneScoped;

  if (entity && entity->is_valid()) {
    writer[name] = entity->name().c_str();
  }
}

auto JsonEntitySerializer::on_component(std::string_view name, flecs::id_t* component) -> void {
  ZoneScoped;

  if (component && *component) {
    flecs::entity comp_entity(world, *component);
    if (comp_entity.is_valid()) {
      writer[name] = comp_entity.name().c_str();
    }
  }
}

auto JsonEntitySerializer::on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void {
  ZoneScoped;

  if (!name.empty()) {
    writer.key(name);
    writer.begin_obj();
    serialize_ops(ops + 1, op_count - 1, base);
    writer.end_obj();
  } else {
    serialize_ops(ops + 1, op_count - 1, base);
  }
}

auto JsonEntitySerializer::on_opaque_value(
  std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
) -> void {
  ZoneScoped;

  if (opaque_type == flecs::Bool) {
    writer[name] = *static_cast<const bool*>(value);
  } else if (opaque_type == flecs::Char) {
    writer[name] = *static_cast<const c8*>(value);
  } else if (opaque_type == flecs::Byte) {
    writer[name] = *static_cast<const u8*>(value);
  } else if (opaque_type == flecs::U8) {
    writer[name] = *static_cast<const u8*>(value);
  } else if (opaque_type == flecs::U16) {
    writer[name] = *static_cast<const u16*>(value);
  } else if (opaque_type == flecs::U32) {
    writer[name] = *static_cast<const u32*>(value);
  } else if (opaque_type == flecs::U64) {
    writer[name] = *static_cast<const u64*>(value);
  } else if (opaque_type == flecs::Uptr) {
    writer[name] = *static_cast<const u64*>(value);
  } else if (opaque_type == flecs::I8) {
    writer[name] = *static_cast<const i8*>(value);
  } else if (opaque_type == flecs::I16) {
    writer[name] = *static_cast<const i16*>(value);
  } else if (opaque_type == flecs::I32) {
    writer[name] = *static_cast<const i32*>(value);
  } else if (opaque_type == flecs::I64) {
    writer[name] = *static_cast<const i64*>(value);
  } else if (opaque_type == flecs::Iptr) {
    writer[name] = *static_cast<const i64*>(value);
  } else if (opaque_type == flecs::F32) {
    writer[name] = *static_cast<const f32*>(value);
  } else if (opaque_type == flecs::F64) {
    writer[name] = *static_cast<const f64*>(value);
  } else if (opaque_type == flecs::String) {
    const auto* str = *static_cast<const c8* const*>(value);
    writer[name] = str ? str : nullptr;
  }
}

} // namespace ox
