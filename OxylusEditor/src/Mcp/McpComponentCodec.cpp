#include "Mcp/McpComponentCodec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <flecs/addons/meta.h>
#include <fmt/format.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <limits>
#include <type_traits>
#include <utility>

#include "Actions/AssetActions.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Mcp/McpRefs.hpp"
#include "Memory/Stack.hpp"
#include "Scene/EntitySerializer.hpp"

namespace ox {
struct IntLayout {
  usize width = 0;
  bool is_signed = false;
};

struct EnumConstant {
  std::string name = {};
  i64 signed_value = 0;
  u64 unsigned_value = 0;
};

struct JsonInt {
  i64 value = 0;
  u64 big_value = 0;
  bool is_big = false;
};

struct EnumBits {
  i64 signed_value = 0;
  u64 unsigned_value = 0;
};

struct PatchContext {
  Scene& scene;
  flecs::world& world;
  flecs::entity_t component = 0;
  flecs::entity_t quat_type = 0;
  flecs::entity_t uuid_type = 0;
  flecs::entity_t string_type = 0;
  std::vector<std::string>& warnings;
};

using PatchResult = std::expected<void, std::string>;

static auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

static auto layout_of(ecs_primitive_kind_t kind) -> option<IntLayout> {
  switch (kind) {
    case EcsU8 : return IntLayout{1, false};
    case EcsU16: return IntLayout{2, false};
    case EcsU32: return IntLayout{4, false};
    case EcsU64: return IntLayout{8, false};
    case EcsI8 : return IntLayout{1, true};
    case EcsI16: return IntLayout{2, true};
    case EcsI32: return IntLayout{4, true};
    case EcsI64: return IntLayout{8, true};
    default    : return nullopt;
  }
}

static auto layout_of(ecs_meta_op_kind_t kind) -> option<IntLayout> {
  switch (kind) {
    case EcsOpU8 : return IntLayout{1, false};
    case EcsOpU16: return IntLayout{2, false};
    case EcsOpU32: return IntLayout{4, false};
    case EcsOpU64: return IntLayout{8, false};
    case EcsOpI8 : return IntLayout{1, true};
    case EcsOpI16: return IntLayout{2, true};
    case EcsOpI32: return IntLayout{4, true};
    case EcsOpI64: return IntLayout{8, true};
    default      : return nullopt;
  }
}

static auto read_enum_bits(const void* ptr, IntLayout layout) -> EnumBits {
  u64 bits = 0;
  std::memcpy(&bits, ptr, layout.width);

  EnumBits out;
  out.unsigned_value = bits;
  if (layout.is_signed) {
    const auto shift = static_cast<u32>(64 - layout.width * 8);
    out.signed_value = static_cast<i64>(bits << shift) >> shift;
  } else {
    out.signed_value = static_cast<i64>(bits);
  }

  return out;
}

static auto enum_constants(flecs::world& world, flecs::entity_t type) -> std::vector<EnumConstant> {
  std::vector<EnumConstant> out;

  const auto* constants = ecs_get(world, type, EcsConstants);
  if (constants == nullptr) {
    return out;
  }

  const auto count = ecs_vec_count(&constants->ordered_constants);
  const auto* items = ecs_vec_first_t(&constants->ordered_constants, ecs_enum_constant_t);
  for (i32 i = 0; i < count; i++) {
    out.push_back(
      EnumConstant{
        .name = items[i].name != nullptr ? items[i].name : "",
        .signed_value = items[i].value,
        .unsigned_value = items[i].value_unsigned,
      }
    );
  }

  return out;
}

static auto constant_matches(const EnumConstant& constant, IntLayout layout, const EnumBits& bits) -> bool {
  return layout.is_signed ? constant.signed_value == bits.signed_value : constant.unsigned_value == bits.unsigned_value;
}

static auto enum_layout(flecs::world& world, flecs::entity_t type) -> IntLayout {
  const auto* enum_info = ecs_get(world, type, EcsEnum);
  if (enum_info != nullptr) {
    const auto* primitive = ecs_get(world, enum_info->underlying_type, EcsPrimitive);
    if (primitive != nullptr) {
      if (const auto layout = layout_of(primitive->kind); layout.has_value()) {
        return *layout;
      }
    }
  }

  return IntLayout{4, true};
}

static auto enum_value_name(flecs::world& world, flecs::entity_t type, IntLayout layout, const void* ptr)
  -> option<std::string> {
  const auto bits = read_enum_bits(ptr, layout);
  for (const auto& constant : enum_constants(world, type)) {
    if (constant_matches(constant, layout, bits)) {
      return constant.name;
    }
  }

  return nullopt;
}

static auto emit_float(JsonWriter& writer, f64 value, bool is_single) -> void {
  if (!std::isfinite(value)) {
    writer.raw("null");
    return;
  }

  writer.raw(is_single ? fmt::format("{}", static_cast<f32>(value)) : fmt::format("{}", value));
}

static auto emit_signed(JsonWriter& writer, i64 value) -> void { writer.raw(fmt::format("{}", value)); }

static auto emit_unsigned(JsonWriter& writer, u64 value) -> void { writer.raw(fmt::format("{}", value)); }

static auto emit_enum_integer(JsonWriter& writer, IntLayout layout, const void* ptr) -> void {
  const auto bits = read_enum_bits(ptr, layout);
  if (layout.is_signed) {
    emit_signed(writer, bits.signed_value);
  } else {
    emit_unsigned(writer, bits.unsigned_value);
  }
}

static auto emit_euler(JsonWriter& writer, const glm::quat& rotation) -> void {
  const auto euler = glm::degrees(glm::eulerAngles(rotation));
  writer.key("euler_deg");
  writer.begin_array();
  emit_float(writer, euler.x, true);
  emit_float(writer, euler.y, true);
  emit_float(writer, euler.z, true);
  writer.end_array();
}

struct McpJsonSerializer : IEntitySerializer {
  JsonWriter& writer;
  flecs::entity_t quat_type = 0;
  flecs::entity_t uuid_type = 0;

  McpJsonSerializer(flecs::world& world_, JsonWriter& writer_)
      : IEntitySerializer(world_),
        writer(writer_),
        quat_type(world_.entity<glm::quat>().id()),
        uuid_type(world_.entity<UUID>().id()) {}

  auto on_primitive(std::string_view name, Primitive primitive) -> void override {
    ZoneScoped;

    writer.key(name);
    std::visit(
      [this](auto* value) {
        using T = std::remove_pointer_t<decltype(value)>;
        if constexpr (std::is_same_v<T, bool>) {
          writer.raw(*value ? "true" : "false");
        } else if constexpr (std::is_same_v<T, c8>) {
          *value == '\0' ? writer.str("") : writer.str(std::string_view(value, 1));
        } else if constexpr (std::is_floating_point_v<T>) {
          emit_float(writer, static_cast<f64>(*value), std::is_same_v<T, f32>);
        } else if constexpr (std::is_signed_v<T>) {
          emit_signed(writer, static_cast<i64>(*value));
        } else {
          emit_unsigned(writer, static_cast<u64>(*value));
        }
      },
      primitive
    );
  }

  auto on_string(std::string_view name, const c8** str) -> void override {
    ZoneScoped;

    writer.key(name);
    if (str != nullptr && *str != nullptr) {
      writer.str(*str);
    } else {
      writer.raw("null");
    }
  }

  auto on_entity(std::string_view name, flecs::entity_t* entity) -> void override {
    ZoneScoped;

    const auto id = entity != nullptr ? *entity : 0;
    writer.key(name);
    if (id != 0 && ecs_is_alive(world, id)) {
      writer.str(entity_ref_text(flecs::entity(world, id)));
    } else {
      writer.raw("null");
    }
  }

  auto on_enum(std::string_view name, ecs_meta_op_kind_t underlying_kind, flecs::entity_t type, void* ptr)
    -> void override {
    ZoneScoped;

    const auto layout = layout_of(underlying_kind);
    if (!layout.has_value() || ptr == nullptr) {
      return;
    }

    writer.key(name);
    if (const auto constant = enum_value_name(world, type, *layout, ptr); constant.has_value()) {
      writer.str(*constant);
    } else {
      emit_enum_integer(writer, *layout, ptr);
    }
  }

  auto on_component(std::string_view name, flecs::id_t* component) -> void override {
    ZoneScoped;

    writer.key(name);
    if (component != nullptr && *component != 0 && ecs_is_alive(world, *component)) {
      writer.str(flecs::entity(world, *component).name().c_str());
    } else {
      writer.raw("null");
    }
  }

  auto on_struct(std::string_view name, flecs::meta::op_t* ops, i32 op_count, void* base) -> void override {
    ZoneScoped;

    if (name.empty()) {
      serialize_ops(ops + 1, op_count - 1, base);
      return;
    }

    writer.key(name);
    writer.begin_obj();
    serialize_ops(ops + 1, op_count - 1, base);
    if (ops[0].type == quat_type) {
      emit_euler(writer, *static_cast<const glm::quat*>(base));
    }
    writer.end_obj();
  }

  auto on_opaque_value(
    std::string_view name, flecs::entity_t field_type, void* field_ptr, flecs::entity_t opaque_type, const void* value
  ) -> void override {
    ZoneScoped;

    if (name.empty()) {
      return;
    }

    if (field_type == uuid_type) {
      const auto& uuid = *static_cast<const UUID*>(field_ptr);
      writer.key(name);
      writer.str(uuid ? uuid.str() : std::string());
      return;
    }

    writer.key(name);
    if (opaque_type == flecs::Bool) {
      writer.raw(*static_cast<const bool*>(value) ? "true" : "false");
    } else if (opaque_type == flecs::U8 || opaque_type == flecs::Byte) {
      emit_unsigned(writer, *static_cast<const u8*>(value));
    } else if (opaque_type == flecs::U16) {
      emit_unsigned(writer, *static_cast<const u16*>(value));
    } else if (opaque_type == flecs::U32) {
      emit_unsigned(writer, *static_cast<const u32*>(value));
    } else if (opaque_type == flecs::U64 || opaque_type == flecs::Uptr) {
      emit_unsigned(writer, *static_cast<const u64*>(value));
    } else if (opaque_type == flecs::I8) {
      emit_signed(writer, *static_cast<const i8*>(value));
    } else if (opaque_type == flecs::I16) {
      emit_signed(writer, *static_cast<const i16*>(value));
    } else if (opaque_type == flecs::I32) {
      emit_signed(writer, *static_cast<const i32*>(value));
    } else if (opaque_type == flecs::I64 || opaque_type == flecs::Iptr) {
      emit_signed(writer, *static_cast<const i64*>(value));
    } else if (opaque_type == flecs::F32) {
      emit_float(writer, *static_cast<const f32*>(value), true);
    } else if (opaque_type == flecs::F64) {
      emit_float(writer, *static_cast<const f64*>(value), false);
    } else if (opaque_type == flecs::String) {
      const auto* str = *static_cast<const c8* const*>(value);
      if (str != nullptr) {
        writer.str(str);
      } else {
        writer.raw("null");
      }
    } else {
      writer.raw("null");
    }
  }
};

struct AssetCollector : IEntitySerializer {
  std::vector<UUID>& out;
  flecs::entity_t uuid_type = 0;

  AssetCollector(flecs::world& world_, std::vector<UUID>& out_)
      : IEntitySerializer(world_),
        out(out_),
        uuid_type(world_.entity<UUID>().id()) {}

  auto on_struct(std::string_view, flecs::meta::op_t* ops, i32 op_count, void* base) -> void override {
    serialize_ops(ops + 1, op_count - 1, base);
  }

  auto on_opaque_value(std::string_view, flecs::entity_t field_type, void* field_ptr, flecs::entity_t, const void*)
    -> void override {
    if (field_type != uuid_type) {
      return;
    }

    const auto& uuid = *static_cast<const UUID*>(field_ptr);
    if (uuid) {
      out.push_back(uuid);
    }
  }
};

auto component_to_json(JsonWriter& writer, flecs::world& world, flecs::id component, const void* data) -> void {
  ZoneScoped;

  writer.begin_obj();
  if (data != nullptr) {
    auto serializer = McpJsonSerializer(world, writer);
    serializer.serialize(flecs::entity(world, component.raw_id()), const_cast<void*>(data));
  }
  writer.end_obj();
}

auto collect_asset_fields(flecs::world& world, flecs::id component, const void* data, std::vector<UUID>& out) -> void {
  ZoneScoped;

  if (data == nullptr) {
    return;
  }

  auto collector = AssetCollector(world, out);
  collector.serialize(flecs::entity(world, component.raw_id()), const_cast<void*>(data));
}

static auto json_type_name(simdjson::dom::element_type type) -> std::string_view {
  switch (type) {
    case simdjson::dom::element_type::ARRAY     : return "array";
    case simdjson::dom::element_type::OBJECT    : return "object";
    case simdjson::dom::element_type::INT64     : return "integer";
    case simdjson::dom::element_type::UINT64    : return "integer";
    case simdjson::dom::element_type::DOUBLE    : return "number";
    case simdjson::dom::element_type::STRING    : return "string";
    case simdjson::dom::element_type::BOOL      : return "boolean";
    case simdjson::dom::element_type::NULL_VALUE: return "null";
  }

  return "unknown";
}

static auto primitive_name(ecs_primitive_kind_t kind) -> std::string_view {
  switch (kind) {
    case EcsBool  : return "bool";
    case EcsChar  : return "char";
    case EcsByte  :
    case EcsU8    : return "u8";
    case EcsU16   : return "u16";
    case EcsU32   : return "u32";
    case EcsU64   :
    case EcsUPtr  : return "u64";
    case EcsI8    : return "i8";
    case EcsI16   : return "i16";
    case EcsI32   : return "i32";
    case EcsI64   :
    case EcsIPtr  : return "i64";
    case EcsF32   : return "f32";
    case EcsF64   : return "f64";
    case EcsString: return "string";
    case EcsEntity: return "entity";
    case EcsId    : return "component";
  }

  return "unknown";
}

static auto mismatch(std::string_view path, std::string_view expected, simdjson::dom::element value)
  -> std::unexpected<std::string> {
  return std::unexpected(fmt::format("{}: expected {}, got {}", path, expected, json_type_name(value.type())));
}

static auto as_number(simdjson::dom::element value) -> option<f64> {
  switch (value.type()) {
    case simdjson::dom::element_type::INT64 : return static_cast<f64>(i64(value));
    case simdjson::dom::element_type::UINT64: return static_cast<f64>(u64(value));
    case simdjson::dom::element_type::DOUBLE: return f64(value);
    default                                 : return nullopt;
  }
}

static auto as_integer(simdjson::dom::element value) -> option<JsonInt> {
  switch (value.type()) {
    case simdjson::dom::element_type::INT64 : return JsonInt{.value = i64(value)};
    case simdjson::dom::element_type::UINT64: {
      const auto unsigned_value = u64(value);
      if (unsigned_value > static_cast<u64>(std::numeric_limits<i64>::max())) {
        return JsonInt{.big_value = unsigned_value, .is_big = true};
      }

      return JsonInt{.value = static_cast<i64>(unsigned_value)};
    }
    case simdjson::dom::element_type::DOUBLE: {
      const auto number = f64(value);
      if (std::trunc(number) != number || std::fabs(number) >= 9.0e18) {
        return nullopt;
      }

      return JsonInt{.value = static_cast<i64>(number)};
    }
    default: return nullopt;
  }
}

template <typename T>
static auto store_int(void* ptr, const JsonInt& value) -> bool {
  if (value.is_big) {
    if constexpr (std::is_same_v<T, u64>) {
      *static_cast<T*>(ptr) = value.big_value;
      return true;
    } else {
      return false;
    }
  }

  if (!std::in_range<T>(value.value)) {
    return false;
  }

  *static_cast<T*>(ptr) = static_cast<T>(value.value);
  return true;
}

static auto store_integer(ecs_primitive_kind_t kind, void* ptr, const JsonInt& value) -> bool {
  switch (kind) {
    case EcsByte:
    case EcsU8  : return store_int<u8>(ptr, value);
    case EcsU16 : return store_int<u16>(ptr, value);
    case EcsU32 : return store_int<u32>(ptr, value);
    case EcsU64 :
    case EcsUPtr: return store_int<u64>(ptr, value);
    case EcsI8  : return store_int<i8>(ptr, value);
    case EcsI16 : return store_int<i16>(ptr, value);
    case EcsI32 : return store_int<i32>(ptr, value);
    case EcsI64 :
    case EcsIPtr: return store_int<i64>(ptr, value);
    default     : return false;
  }
}

static auto is_integer_kind(ecs_primitive_kind_t kind) -> bool {
  switch (kind) {
    case EcsByte:
    case EcsU8  :
    case EcsU16 :
    case EcsU32 :
    case EcsU64 :
    case EcsUPtr:
    case EcsI8  :
    case EcsI16 :
    case EcsI32 :
    case EcsI64 :
    case EcsIPtr: return true;
    default     : return false;
  }
}

static auto patch_value(
  PatchContext& ctx,
  const std::string& path,
  std::string_view field_name,
  bool is_top_level,
  flecs::entity_t type,
  void* ptr,
  simdjson::dom::element value
) -> PatchResult;

static auto member_path(const std::string& path, std::string_view name) -> std::string {
  return fmt::format("{}.{}", path, name);
}

struct VectorInfo {
  usize count = 0;
  std::string_view prefix = {};
};

static auto vector_info(flecs::world& world, flecs::entity_t type) -> option<VectorInfo> {
  const auto* members_info = ecs_get(world, type, EcsStruct);
  if (members_info == nullptr) {
    return nullopt;
  }

  const auto count = static_cast<usize>(ecs_vec_count(&members_info->members));
  if (count < 2 || count > 4) {
    return nullopt;
  }

  constexpr std::string_view NAMES[] = {"x", "y", "z", "w"};
  const auto* members = ecs_vec_first_t(&members_info->members, ecs_member_t);
  const auto first_type = members[0].type;
  for (usize i = 0; i < count; i++) {
    if (members[i].type != first_type || NAMES[i] != members[i].name || members[i].count > 1) {
      return nullopt;
    }
  }

  const auto* primitive = ecs_get(world, first_type, EcsPrimitive);
  if (primitive == nullptr) {
    return nullopt;
  }

  switch (primitive->kind) {
    case EcsF32: return VectorInfo{count, "vec"};
    case EcsF64: return VectorInfo{count, "dvec"};
    case EcsI32: return VectorInfo{count, "ivec"};
    case EcsU32: return VectorInfo{count, "uvec"};
    default    : return nullopt;
  }
}

static auto struct_member_names(flecs::world& world, flecs::entity_t type) -> std::string {
  std::string names;
  const auto* members_info = ecs_get(world, type, EcsStruct);
  if (members_info == nullptr) {
    return names;
  }

  const auto count = ecs_vec_count(&members_info->members);
  const auto* members = ecs_vec_first_t(&members_info->members, ecs_member_t);
  for (i32 i = 0; i < count; i++) {
    names += fmt::format("{}{}", names.empty() ? "" : ", ", members[i].name);
  }

  return names;
}

static auto patch_struct(
  PatchContext& ctx,
  const std::string& path,
  bool is_top_level,
  flecs::entity_t type,
  void* ptr,
  simdjson::dom::element value
) -> PatchResult {
  const auto* members_info = ecs_get(ctx.world, type, EcsStruct);
  if (members_info == nullptr) {
    return std::unexpected(fmt::format("{}: has no reflection data, not editable through MCP", path));
  }

  const auto count = static_cast<usize>(ecs_vec_count(&members_info->members));
  const auto* members = ecs_vec_first_t(&members_info->members, ecs_member_t);

  if (value.type() == simdjson::dom::element_type::ARRAY) {
    const auto vector = vector_info(ctx.world, type);
    if (!vector.has_value()) {
      return mismatch(path, "an object", value);
    }

    const auto array = simdjson::dom::array(value);
    if (array.size() != vector->count) {
      return std::unexpected(
        fmt::format("{}: expected an array of {} numbers, got {} elements", path, vector->count, array.size())
      );
    }

    for (usize i = 0; i < count; i++) {
      const auto element_path = fmt::format("{}[{}]", path, i);
      auto* member_ptr = static_cast<u8*>(ptr) + members[i].offset;
      if (
        const auto
          result = patch_value(ctx, element_path, members[i].name, false, members[i].type, member_ptr, array.at(i));
        !result
      ) {
        return result;
      }
    }

    return {};
  }

  if (value.type() != simdjson::dom::element_type::OBJECT) {
    return mismatch(path, "an object", value);
  }

  for (const auto& [key, member_value] : simdjson::dom::object(value)) {
    usize index = count;
    for (usize i = 0; i < count; i++) {
      if (key == members[i].name) {
        index = i;
        break;
      }
    }

    if (index == count) {
      return std::unexpected(
        fmt::format("{}: unknown field, valid: {}", member_path(path, key), struct_member_names(ctx.world, type))
      );
    }

    const auto& member = members[index];
    const auto child_path = member_path(path, key);
    if (member.count > 1) {
      return std::unexpected(fmt::format("{}: not editable through MCP yet", child_path));
    }

    auto* member_ptr = static_cast<u8*>(ptr) + member.offset;
    if (
      const auto
        result = patch_value(ctx, child_path, member.name, is_top_level, member.type, member_ptr, member_value);
      !result
    ) {
      return result;
    }
  }

  return {};
}

static auto patch_quat(PatchContext& ctx, const std::string& path, void* ptr, simdjson::dom::element value)
  -> PatchResult {
  auto* rotation = static_cast<glm::quat*>(ptr);

  if (value.type() == simdjson::dom::element_type::ARRAY) {
    const auto array = simdjson::dom::array(value);
    if (array.size() != 4) {
      return std::unexpected(
        fmt::format("{}: expected an array of 4 numbers [x,y,z,w], got {} elements", path, array.size())
      );
    }

    glm::quat next = *rotation;
    for (usize i = 0; i < 4; i++) {
      const auto number = as_number(array.at(i));
      if (!number.has_value()) {
        return mismatch(fmt::format("{}[{}]", path, i), "a number", array.at(i));
      }

      next[static_cast<i32>(i)] = static_cast<f32>(*number);
    }

    if (glm::dot(next, next) <= 0.0f) {
      return std::unexpected(fmt::format("{}: a quaternion can't be all zero", path));
    }

    *rotation = glm::normalize(next);
    return {};
  }

  if (value.type() != simdjson::dom::element_type::OBJECT) {
    return mismatch(path, "an object {x,y,z,w} or {euler_deg}", value);
  }

  const auto object = simdjson::dom::object(value);
  bool has_components = false;
  bool has_euler = false;
  for (const auto& [key, ignored] : object) {
    if (key == "euler_deg") {
      has_euler = true;
    } else if (key == "x" || key == "y" || key == "z" || key == "w") {
      has_components = true;
    } else {
      return std::unexpected(fmt::format("{}: unknown field, valid: x, y, z, w, euler_deg", member_path(path, key)));
    }
  }

  if (has_euler && has_components) {
    return std::unexpected(fmt::format("{}: give either x,y,z,w or euler_deg, not both", path));
  }

  if (has_euler) {
    const auto euler_path = member_path(path, "euler_deg");
    simdjson::dom::element euler_value;
    if (object.at_key("euler_deg").get(euler_value)) {
      return std::unexpected(fmt::format("{}: missing", euler_path));
    }

    glm::vec3 euler = {};
    if (euler_value.type() == simdjson::dom::element_type::ARRAY) {
      const auto array = simdjson::dom::array(euler_value);
      if (array.size() != 3) {
        return std::unexpected(
          fmt::format("{}: expected an array of 3 numbers, got {} elements", euler_path, array.size())
        );
      }

      for (usize i = 0; i < 3; i++) {
        const auto number = as_number(array.at(i));
        if (!number.has_value()) {
          return mismatch(fmt::format("{}[{}]", euler_path, i), "a number", array.at(i));
        }

        euler[static_cast<i32>(i)] = static_cast<f32>(*number);
      }
    } else if (euler_value.type() == simdjson::dom::element_type::OBJECT) {
      constexpr std::string_view AXES[] = {"x", "y", "z"};
      const auto euler_object = simdjson::dom::object(euler_value);
      for (const auto& [axis, ignored] : euler_object) {
        if (axis != "x" && axis != "y" && axis != "z") {
          return std::unexpected(fmt::format("{}: unknown field, valid: x, y, z", member_path(euler_path, axis)));
        }
      }

      for (usize i = 0; i < 3; i++) {
        simdjson::dom::element axis_value;
        if (euler_object.at_key(AXES[i]).get(axis_value)) {
          return std::unexpected(fmt::format("{}: needs x, y and z", euler_path));
        }

        const auto number = as_number(axis_value);
        if (!number.has_value()) {
          return mismatch(member_path(euler_path, AXES[i]), "a number", axis_value);
        }

        euler[static_cast<i32>(i)] = static_cast<f32>(*number);
      }
    } else {
      return mismatch(euler_path, "an array [x,y,z] or object", euler_value);
    }

    *rotation = glm::normalize(glm::quat(glm::radians(euler)));
    return {};
  }

  glm::quat next = *rotation;
  constexpr std::string_view AXES[] = {"x", "y", "z", "w"};
  for (usize i = 0; i < 4; i++) {
    simdjson::dom::element component_value;
    if (object.at_key(AXES[i]).get(component_value)) {
      continue;
    }

    const auto number = as_number(component_value);
    if (!number.has_value()) {
      return mismatch(member_path(path, AXES[i]), "a number", component_value);
    }

    next[static_cast<i32>(i)] = static_cast<f32>(*number);
  }

  if (glm::dot(next, next) <= 0.0f) {
    return std::unexpected(fmt::format("{}: a quaternion can't be all zero", path));
  }

  *rotation = glm::normalize(next);
  (void)ctx;
  return {};
}

static auto patch_enum(
  PatchContext& ctx, const std::string& path, flecs::entity_t type, void* ptr, simdjson::dom::element value
) -> PatchResult {
  const auto layout = enum_layout(ctx.world, type);
  const auto constants = enum_constants(ctx.world, type);

  const auto valid_names = [&] {
    std::string names;
    for (const auto& constant : constants) {
      names += fmt::format("{}{}", names.empty() ? "" : ", ", constant.name);
    }

    return names;
  };

  const EnumConstant* match = nullptr;
  if (value.type() == simdjson::dom::element_type::STRING) {
    const auto text = lower(std::string_view(value));
    for (const auto& constant : constants) {
      if (lower(constant.name) == text) {
        match = &constant;
        break;
      }
    }

    if (match == nullptr) {
      return std::unexpected(
        fmt::format("{}: unknown value '{}', valid: {}", path, std::string_view(value), valid_names())
      );
    }
  } else {
    const auto integer = as_integer(value);
    if (!integer.has_value()) {
      return mismatch(path, "an enum name or integer", value);
    }

    for (const auto& constant : constants) {
      const auto is_equal = integer->is_big
                              ? (!layout.is_signed && constant.unsigned_value == integer->big_value)
                              : (layout.is_signed ? constant.signed_value == integer->value
                                                  : integer->value >= 0 &&
                                                      constant.unsigned_value == static_cast<u64>(integer->value));
      if (is_equal) {
        match = &constant;
        break;
      }
    }

    if (match == nullptr) {
      return std::unexpected(
        fmt::format(
          "{}: {} is not a valid value, valid: {}",
          path,
          integer->is_big ? integer->big_value : static_cast<u64>(integer->value),
          valid_names()
        )
      );
    }
  }

  const u64 bits = layout.is_signed ? static_cast<u64>(match->signed_value) : match->unsigned_value;
  std::memcpy(ptr, &bits, layout.width);
  return {};
}

static auto patch_uuid(
  PatchContext& ctx,
  const std::string& path,
  std::string_view field_name,
  bool is_top_level,
  void* ptr,
  simdjson::dom::element value
) -> PatchResult {
  if (is_top_level && field_name == "model_uuid" && ctx.component == ctx.world.entity<MeshComponent>().id()) {
    return std::unexpected(
      fmt::format("{}: models are placed with entity_create_from_asset, which builds the mesh hierarchy", path)
    );
  }

  auto& uuid = *static_cast<UUID*>(ptr);
  if (value.type() == simdjson::dom::element_type::NULL_VALUE) {
    uuid = UUID(nullptr);
    return {};
  }

  if (value.type() != simdjson::dom::element_type::STRING) {
    return mismatch(path, "an asset uuid or path string, or null", value);
  }

  const std::string_view text = value;
  if (text.empty()) {
    uuid = UUID(nullptr);
    return {};
  }

  const auto asset = resolve_asset(text);
  if (!asset) {
    return std::unexpected(fmt::format("{}: {}", path, asset.error()));
  }

  const auto expected_type = expected_asset_type(field_name);
  if (expected_type != AssetType::None) {
    auto actual_type = AssetType::None;
    {
      const auto guard = App::mod<AssetManager>().get_asset(*asset);
      if (guard) {
        actual_type = guard->type;
      }
    }

    if (actual_type != expected_type) {
      ctx.warnings.push_back(
        fmt::format(
          "{}: asset {} is a {}, the field '{}' usually holds a {}",
          path,
          asset->str(),
          AssetManager::to_asset_type_sv(actual_type),
          field_name,
          AssetManager::to_asset_type_sv(expected_type)
        )
      );
    }
  }

  uuid = *asset;
  return {};
}

static auto patch_primitive(
  PatchContext& ctx, const std::string& path, ecs_primitive_kind_t kind, void* ptr, simdjson::dom::element value
) -> PatchResult {
  memory::ScopedStack stack;

  const auto is_null = value.type() == simdjson::dom::element_type::NULL_VALUE;

  if (kind == EcsBool) {
    if (value.type() != simdjson::dom::element_type::BOOL) {
      return mismatch(path, "a boolean", value);
    }

    *static_cast<bool*>(ptr) = bool(value);
    return {};
  }

  if (kind == EcsChar) {
    if (value.type() != simdjson::dom::element_type::STRING || std::string_view(value).size() != 1) {
      return mismatch(path, "a one character string", value);
    }

    *static_cast<c8*>(ptr) = std::string_view(value)[0];
    return {};
  }

  if (kind == EcsF32 || kind == EcsF64) {
    const auto number = as_number(value);
    if (!number.has_value()) {
      return mismatch(path, "a number", value);
    }

    if (kind == EcsF32) {
      const auto narrowed = static_cast<f32>(*number);
      if (!std::isfinite(narrowed)) {
        return std::unexpected(fmt::format("{}: {} is out of range for f32", path, *number));
      }

      *static_cast<f32*>(ptr) = narrowed;
    } else {
      *static_cast<f64*>(ptr) = *number;
    }

    return {};
  }

  if (is_integer_kind(kind)) {
    const auto integer = as_integer(value);
    if (!integer.has_value()) {
      return mismatch(
        path,
        value.type() == simdjson::dom::element_type::DOUBLE ? "an integral number" : "an integer",
        value
      );
    }

    if (!store_integer(kind, ptr, *integer)) {
      return std::unexpected(
        fmt::format(
          "{}: {} is out of range for {}",
          path,
          integer->is_big ? integer->big_value : static_cast<u64>(integer->value),
          primitive_name(kind)
        )
      );
    }

    return {};
  }

  if (kind == EcsString) {
    auto** field = static_cast<c8**>(ptr);
    if (!is_null && value.type() != simdjson::dom::element_type::STRING) {
      return mismatch(path, "a string or null", value);
    }

    ecs_os_free(*field);
    *field = is_null ? nullptr : ecs_os_strdup(stack.null_terminate_cstr(std::string_view(value)));
    return {};
  }

  if (kind == EcsEntity) {
    if (is_null) {
      *static_cast<flecs::entity_t*>(ptr) = 0;
      return {};
    }

    if (value.type() != simdjson::dom::element_type::STRING) {
      return mismatch(path, "an entity reference string or null", value);
    }

    const auto entity = resolve_entity(ctx.scene, value);
    if (!entity) {
      return std::unexpected(fmt::format("{}: {}", path, entity.error()));
    }

    *static_cast<flecs::entity_t*>(ptr) = entity->id();
    return {};
  }

  if (kind == EcsId) {
    if (is_null) {
      *static_cast<flecs::id_t*>(ptr) = 0;
      return {};
    }

    if (value.type() != simdjson::dom::element_type::STRING) {
      return mismatch(path, "a component name or null", value);
    }

    const auto component = resolve_component(ctx.scene, value);
    if (!component) {
      return std::unexpected(fmt::format("{}: {}", path, component.error()));
    }

    *static_cast<flecs::id_t*>(ptr) = component->raw_id();
    return {};
  }

  return std::unexpected(fmt::format("{}: not editable through MCP yet", path));
}

static auto patch_value(
  PatchContext& ctx,
  const std::string& path,
  std::string_view field_name,
  bool is_top_level,
  flecs::entity_t type,
  void* ptr,
  simdjson::dom::element value
) -> PatchResult {
  if (type == ctx.uuid_type) {
    return patch_uuid(ctx, path, field_name, is_top_level, ptr, value);
  }

  if (type == ctx.string_type) {
    if (value.type() != simdjson::dom::element_type::STRING) {
      return mismatch(path, "a string", value);
    }

    *static_cast<std::string*>(ptr) = std::string(std::string_view(value));
    return {};
  }

  if (type == ctx.quat_type) {
    return patch_quat(ctx, path, ptr, value);
  }

  const auto* type_info = ecs_get(ctx.world, type, EcsType);
  if (type_info == nullptr) {
    return std::unexpected(fmt::format("{}: has no reflection data, not editable through MCP", path));
  }

  switch (type_info->kind) {
    case EcsPrimitiveType: {
      const auto* primitive = ecs_get(ctx.world, type, EcsPrimitive);
      return patch_primitive(ctx, path, primitive->kind, ptr, value);
    }
    case EcsEnumType  : return patch_enum(ctx, path, type, ptr, value);
    case EcsStructType: return patch_struct(ctx, path, false, type, ptr, value);
    default           : return std::unexpected(fmt::format("{}: not editable through MCP yet", path));
  }
}

auto patch_component(
  Scene& scene, flecs::entity, flecs::id component, simdjson::dom::element patch, ComponentBytes& target
) -> std::expected<std::vector<std::string>, std::string> {
  ZoneScoped;

  const auto name = component_name(component);
  if (target.is_empty()) {
    return std::unexpected(fmt::format("{}: has no data to edit", name));
  }

  if (patch.type() != simdjson::dom::element_type::OBJECT) {
    return mismatch(name, "an object of fields", patch);
  }

  std::vector<std::string> warnings;
  auto ctx = PatchContext{
    .scene = scene,
    .world = scene.world,
    .component = component.raw_id(),
    .quat_type = scene.world.entity<glm::quat>().id(),
    .uuid_type = scene.world.entity<UUID>().id(),
    .string_type = scene.world.entity<std::string>().id(),
    .warnings = warnings,
  };

  if (const auto result = patch_struct(ctx, name, true, component.raw_id(), target.mut_data(), patch); !result) {
    return std::unexpected(result.error());
  }

  return warnings;
}

static auto write_field_schema(
  JsonWriter& writer, Scene& scene, std::string_view name, flecs::entity_t type, option<simdjson::dom::element> defaults
) -> void;

static auto write_struct_fields(
  JsonWriter& writer, Scene& scene, flecs::entity_t type, option<simdjson::dom::element> defaults
) -> void {
  writer.begin_array();

  const auto* members_info = ecs_get(scene.world, type, EcsStruct);
  if (members_info != nullptr) {
    const auto count = ecs_vec_count(&members_info->members);
    const auto* members = ecs_vec_first_t(&members_info->members, ecs_member_t);
    for (i32 i = 0; i < count; i++) {
      option<simdjson::dom::element> member_default = nullopt;
      if (defaults.has_value()) {
        simdjson::dom::element element;
        if (!simdjson::dom::object(*defaults).at_key(members[i].name).get(element)) {
          member_default = element;
        }
      }

      if (members[i].count > 1) {
        writer.begin_obj();
        writer["name"] = std::string_view(members[i].name);
        writer["type"] = "array";
        writer["readonly"] = true;
        writer.end_obj();
        continue;
      }

      write_field_schema(writer, scene, members[i].name, members[i].type, member_default);
    }
  }

  writer.end_array();
}

static auto write_field_schema(
  JsonWriter& writer, Scene& scene, std::string_view name, flecs::entity_t type, option<simdjson::dom::element> defaults
) -> void {
  auto& world = scene.world;

  writer.begin_obj();
  writer["name"] = name;

  const auto write_default = [&] {
    if (defaults.has_value()) {
      writer.key("default");
      writer.raw(simdjson::minify(*defaults));
    }
  };

  if (type == world.entity<UUID>().id()) {
    writer["type"] = "uuid";
    if (const auto asset_type = expected_asset_type(name); asset_type != AssetType::None) {
      writer["asset_type"] = AssetManager::to_asset_type_sv(asset_type);
    }
    write_default();
    writer.end_obj();
    return;
  }

  if (type == world.entity<std::string>().id()) {
    writer["type"] = "string";
    write_default();
    writer.end_obj();
    return;
  }

  if (type == world.entity<glm::quat>().id()) {
    writer["type"] = "quat";
    write_default();
    writer.end_obj();
    return;
  }

  const auto* type_info = ecs_get(world, type, EcsType);
  if (type_info == nullptr) {
    writer["type"] = "opaque";
    writer["readonly"] = true;
    writer.end_obj();
    return;
  }

  switch (type_info->kind) {
    case EcsPrimitiveType: {
      const auto* primitive = ecs_get(world, type, EcsPrimitive);
      writer["type"] = primitive_name(primitive->kind);
      write_default();
    } break;
    case EcsEnumType: {
      writer["type"] = "enum";
      writer.key("constants");
      writer.begin_array();
      for (const auto& constant : enum_constants(world, type)) {
        writer.str(constant.name);
      }
      writer.end_array();
      write_default();
    } break;
    case EcsStructType: {
      if (const auto vector = vector_info(world, type); vector.has_value()) {
        writer["type"] = fmt::format("{}{}", vector->prefix, vector->count);
        write_default();
      } else {
        writer["type"] = "struct";
        writer.key("fields");
        write_struct_fields(writer, scene, type, defaults);
      }
    } break;
    case EcsBitmaskType: {
      writer["type"] = "bitmask";
      writer["readonly"] = true;
    } break;
    case EcsArrayType: {
      writer["type"] = "array";
      writer["readonly"] = true;
    } break;
    case EcsVectorType: {
      writer["type"] = "vector";
      writer["readonly"] = true;
    } break;
    case EcsOpaqueType: {
      writer["type"] = "opaque";
      writer["readonly"] = true;
    } break;
  }

  writer.end_obj();
}

auto component_schema_json(JsonWriter& writer, Scene& scene, flecs::id component) -> void {
  ZoneScoped;

  auto& world = scene.world;
  const auto type = component.raw_id();

  writer.begin_obj();
  writer["component"] = component_name(component);
  writer["path"] = std::string_view(component.entity().path().c_str());

  const auto* type_info = ecs_get_type_info(world, type);
  const auto is_tag = type_info == nullptr || type_info->size == 0;
  writer["is_tag"] = is_tag;

  std::string defaults_json;
  if (!is_tag) {
    if (auto* instance = ecs_value_new(world, type); instance != nullptr) {
      JsonWriter defaults_writer;
      defaults_writer.is_pretty = false;
      component_to_json(defaults_writer, world, component, instance);
      defaults_json = defaults_writer.stream.str();
      ecs_value_free(world, type, instance);
    }
  }

  simdjson::dom::parser parser;
  option<simdjson::dom::element> defaults = nullopt;
  if (!defaults_json.empty()) {
    simdjson::dom::element root;
    if (!parser.parse(defaults_json).get(root)) {
      defaults = root;
    }
  }

  writer.key("fields");
  write_struct_fields(writer, scene, type, defaults);
  writer.end_obj();
}
} // namespace ox
