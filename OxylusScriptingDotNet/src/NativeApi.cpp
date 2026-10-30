#include <ankerl/svector.h>
#include <ankerl/unordered_dense.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <flecs.h>
#include <new>
#include <string_view>
#include <vector>

#include "DotNetHosts.hpp"
#include "Memory/Stack.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"
#include "Scripting/DotNetScripting.hpp"
#include "Utils/Log.hpp"

namespace ox {
// C# reads it field for field as Oxylus.Contact
static_assert(sizeof(PhysicsContact) == 48 && alignof(PhysicsContact) == 8);
static_assert(offsetof(PhysicsContact, point) == 16 && offsetof(PhysicsContact, normal) == 32);
// and these as their Interop structs in Abi.cs
static_assert(sizeof(OxTime) == 24 && offsetof(OxTime, frame) == 16);
static_assert(sizeof(OxInputCode) == 8 && sizeof(OxMouseState) == 24);
static_assert(sizeof(OxRay) == 40 && offsetof(OxRay, ignore_entity) == 32);
static_assert(sizeof(OxRayHit) == 40 && offsetof(OxRayHit, normal) == 24);
static_assert(sizeof(OxBodyState) == 144 && offsetof(OxBodyState, angular_velocity) == 48);
static_assert(offsetof(OxBodyState, bounds_max) == 96 && offsetof(OxBodyState, accumulated_torque) == 128);
static_assert(sizeof(OxBodyArgs) == 48 && offsetof(OxBodyArgs, rotation) == 32);
static_assert(sizeof(OxVehicleState) == 16 && sizeof(OxWheelState) == 12);
static_assert(sizeof(OxCharacterState) == 40 && offsetof(OxCharacterState, ground_normal) == 28);
static_assert(sizeof(OxAssetId) == 16 && sizeof(OxAssetInfo) == 16 && sizeof(OxAudioState) == 16);
static_assert(sizeof(OxValue) == 40 && offsetof(OxValue, text) == 24);
static_assert(sizeof(OxUiEvent) == 40 && offsetof(OxUiEvent, mouse_x) == 32);
static_assert(sizeof(OxNetClientState) == 32 && offsetof(OxNetClientState, rtt) == 16);
static_assert(sizeof(OxVec4) == 16 && sizeof(OxCinematicState) == 8 && sizeof(OxWindowSize) == 16);
static_assert(sizeof(OxMaterialData) == 160 && offsetof(OxMaterialData, alpha_mode) == 60);
static_assert(offsetof(OxMaterialData, flip_normal_y) == 72 && offsetof(OxMaterialData, albedo_texture) == 80);

static auto component_dbs = ankerl::unordered_dense::map<ecs_world_t*, ComponentDB*>{};
static auto frame_time = OxTime{};
static auto loaded_managed_api = static_cast<const OxManagedApi*>(nullptr);

static auto to_view(OxStringView view) -> std::string_view { return std::string_view(view.data, view.length); }

static auto native_log(OxLogLevel level, const c8* message, usize length) -> void {
  const auto text = std::string_view(message, length);
  switch (level) {
    case OxLogLevel::Trace: OX_LOG_TRACE("{}", text); break;
    case OxLogLevel::Debug: OX_LOG_DEBUG("{}", text); break;
    case OxLogLevel::Info : OX_LOG_INFO("{}", text); break;
    case OxLogLevel::Warn : OX_LOG_WARN("{}", text); break;
    case OxLogLevel::Error: OX_LOG_ERROR("{}", text); break;
  }
}

static auto lookup(ecs_world_t* world, OxStringView path) -> u64 {
  memory::ScopedStack stack;
  return ecs_lookup(world, stack.null_terminate_cstr(to_view(path)));
}

static auto get_name(ecs_world_t* world, u64 entity, OxStringView* name) -> void {
  const auto* cstr = ecs_get_name(world, entity);
  *name = cstr ? OxStringView{.data = cstr, .length = std::strlen(cstr)} : OxStringView{};
}

static auto set_name(ecs_world_t* world, u64 entity, OxStringView name) -> void {
  memory::ScopedStack stack;
  ecs_set_name(world, entity, name.length ? stack.null_terminate_cstr(to_view(name)) : nullptr);
}

static auto get_path(ecs_world_t* world, u64 entity, c8* out, u32 capacity) -> u32 {
  auto* path = ecs_get_path_w_sep(world, 0, entity, ".", nullptr);
  if (!path) {
    return 0;
  }

  const auto length = std::strlen(path);
  if (out) {
    std::memcpy(out, path, std::min<usize>(length, capacity));
  }

  ecs_os_free(path);
  return static_cast<u32>(length);
}

static auto children_iter(ecs_world_t* world, u64 parent, void* iter) -> void {
  new (iter) ecs_iter_t(ecs_children(world, parent));
}

// flecs finishes the iterator itself when it runs out
static auto children_next(void* iter, const u64** entities, i32* count) -> bool {
  auto* it = static_cast<ecs_iter_t*>(iter);
  if (!ecs_children_next(it)) {
    return false;
  }

  *entities = it->entities;
  *count = it->count;
  return true;
}

static auto construct_defaults(void* ptr, i32 count, const ecs_type_info_t* type_info) -> void {
  const auto& defaults = *static_cast<const std::vector<u8>*>(type_info->hooks.ctx);
  for (auto* element = static_cast<u8*>(ptr); count > 0; count--, element += type_info->size) {
    std::memcpy(element, defaults.data(), defaults.size());
  }
}

static auto free_defaults(void* ctx) -> void { delete static_cast<std::vector<u8>*>(ctx); }

static auto set_component_defaults(ecs_world_t* world, u64 component, const void* defaults, u32 size) -> OxStatus {
  ZoneScoped;

  const auto* info = static_cast<const EcsComponent*>(ecs_get_id(world, component, ecs_id(EcsComponent)));
  if (!info || static_cast<u32>(info->size) != size) {
    return OxStatus::InvalidArgument;
  }

  // a reload attaches the same component again, flecs keeps the hook so only what it copies changes
  const auto* existing = ecs_get_hooks_id(world, component);
  const auto is_ours = existing && existing->ctor == construct_defaults;
  if (is_ours) {
    auto& bytes = *static_cast<std::vector<u8>*>(existing->ctx);
    if (defaults) {
      std::memcpy(bytes.data(), defaults, size);
    } else {
      std::ranges::fill(bytes, u8{0});
    }

    return OxStatus::Ok;
  }

  if (!defaults) {
    return OxStatus::Ok;
  }

  auto hooks = existing ? *existing : ecs_type_hooks_t{};
  hooks.ctor = construct_defaults;
  hooks.ctx = new std::vector<u8>(static_cast<const u8*>(defaults), static_cast<const u8*>(defaults) + size);
  hooks.ctx_free = free_defaults;
  ecs_set_hooks_id(world, component, &hooks);
  return OxStatus::Ok;
}

static auto native_define_component(
  ecs_world_t* world, OxStringView path, const OxComponentField* fields, i32 field_count
) -> u64 {
  ZoneScoped;

  constexpr auto LAST_FIELD_TYPE = static_cast<i32>(ComponentFieldType::Quat);

  auto component_fields = ankerl::svector<ComponentField, 16>();
  for (const auto& field : std::span(fields, static_cast<usize>(field_count))) {
    if (field.type < 0 || field.type > LAST_FIELD_TYPE) {
      OX_LOG_ERROR("Component {} has a field of unknown type {}", to_view(path), field.type);
      return 0;
    }

    component_fields.push_back({.name = to_view(field.name), .type = static_cast<ComponentFieldType>(field.type)});
  }

  // C# paths are "." separated like every lookup here, the C++ API that defines the component scopes with "::"
  auto scoped_path = std::string(to_view(path));
  for (auto dot = scoped_path.find('.'); dot != std::string::npos; dot = scoped_path.find('.', dot + 2)) {
    scoped_path.replace(dot, 1, "::");
  }

  const auto it = component_dbs.find(world);
  auto cpp_world = flecs::world(world);
  return define_component(cpp_world, it != component_dbs.end() ? it->second : nullptr, scoped_path, component_fields);
}

static auto find_member(const EcsStruct& members, std::string_view name) -> const ecs_member_t* {
  for (const auto& member : std::span(ecs_vec_first_t(&members.members, ecs_member_t), members.members.count)) {
    if (name == member.name) {
      return &member;
    }
  }

  return nullptr;
}

static auto check_layout(ecs_world_t* world, u64 component, const OxComponentLayout& layout) -> bool {
  const auto path = to_view(layout.path);
  // tags have no storage, often not even an EcsComponent, and the managed side describes them with size 0
  const auto* info = static_cast<const EcsComponent*>(ecs_get_id(world, component, ecs_id(EcsComponent)));
  if (!info || info->size == 0) {
    if (layout.size != 0) {
      OX_LOG_ERROR("{} is a tag natively but has {} bytes in C#", path, layout.size);
      return false;
    }

    return true;
  }

  if (static_cast<u32>(info->size) != layout.size || static_cast<u32>(info->alignment) != layout.alignment) {
    OX_LOG_ERROR(
      "{} is {} bytes aligned to {} natively but {} aligned to {} in C#",
      path,
      info->size,
      info->alignment,
      layout.size,
      layout.alignment
    );
    return false;
  }

  if (layout.member_count == 0) {
    return true;
  }

  const auto* members = static_cast<const EcsStruct*>(ecs_get_id(world, component, ecs_id(EcsStruct)));
  if (!members) {
    OX_LOG_ERROR("{} has no reflected members natively", path);
    return false;
  }

  auto is_matching = true;
  for (const auto& managed : std::span(layout.members, layout.member_count)) {
    const auto name = to_view(managed.name);
    const auto* member = find_member(*members, name);
    if (!member) {
      OX_LOG_ERROR("{} has no member {} natively", path, name);
      is_matching = false;
      continue;
    }

    const auto* type_info = static_cast<const EcsComponent*>(ecs_get_id(world, member->type, ecs_id(EcsComponent)));
    const auto size = type_info ? static_cast<u32>(type_info->size * std::max(member->count, 1)) : 0_u32;
    if (static_cast<u32>(member->offset) != managed.offset || size != managed.size) {
      OX_LOG_ERROR(
        "{}.{} is at {} with {} bytes natively but at {} with {} bytes in C#",
        path,
        name,
        member->offset,
        size,
        managed.offset,
        managed.size
      );
      is_matching = false;
    }
  }

  return is_matching;
}

static auto bind_components(ecs_world_t* world, const OxComponentLayout* layouts, i32 layout_count, u64* ids)
  -> OxStatus {
  ZoneScoped;

  auto status = OxStatus::Ok;
  for (i32 i = 0; i < layout_count; i++) {
    ids[i] = lookup(world, layouts[i].path);
    // not in this world, a module it never imported, so the component just stays unusable here
    if (ids[i] == 0) {
      continue;
    }

    if (!check_layout(world, ids[i], layouts[i])) {
      ids[i] = 0;
      status = OxStatus::LayoutMismatch;
    }
  }

  return status;
}

static auto to_phase(OxPhase phase) -> u64 {
  switch (phase) {
    case OxPhase::OnLoad    : return EcsOnLoad;
    case OxPhase::PostLoad  : return EcsPostLoad;
    case OxPhase::PreUpdate : return EcsPreUpdate;
    case OxPhase::OnUpdate  : return EcsOnUpdate;
    case OxPhase::OnValidate: return EcsOnValidate;
    case OxPhase::PostUpdate: return EcsPostUpdate;
    case OxPhase::PreStore  : return EcsPreStore;
    case OxPhase::OnStore   : return EcsOnStore;
  }

  return 0;
}

static auto fill_terms(ecs_term_t* out, const OxTerm* terms, i32 term_count) -> bool {
  if (term_count < 0 || term_count > FLECS_TERM_COUNT_MAX) {
    OX_LOG_ERROR("A C# query has {} terms, flecs takes at most {}", term_count, FLECS_TERM_COUNT_MAX);
    return false;
  }

  for (i32 i = 0; i < term_count; i++) {
    if (terms[i].id == 0) {
      OX_LOG_ERROR("A C# query uses a component this world doesn't have");
      return false;
    }

    out[i].id = terms[i].id;
    out[i].inout = terms[i].access == OxTermAccess::In    ? EcsIn
                   : terms[i].access == OxTermAccess::Out ? EcsOut
                                                          : EcsInOut;
    // owned only, so every field is an array the managed side can span over
    out[i].src.id = EcsSelf;
  }

  return true;
}

static auto fill_view(ecs_iter_t* it, OxTableView* view, void** fields) -> void {
  for (i8 i = 0; i < it->field_count; i++) {
    fields[i] = ecs_field_w_size(it, static_cast<usize>(ecs_field_size(it, i)), i);
  }

  *view = OxTableView{
    .world = it->real_world,
    .system = it->system,
    .entities = it->entities,
    .fields = fields,
    .count = it->count,
    .delta_time = it->delta_time,
  };
}

static auto system_callback(ecs_iter_t* it) -> void {
  auto fields = std::array<void*, FLECS_TERM_COUNT_MAX>{};
  auto view = OxTableView{};
  fill_view(it, &view, fields.data());
  reinterpret_cast<OxSystemRunFn>(it->callback_ctx)(&view);
}

static auto create_system(
  ecs_world_t* world, OxStringView name, OxPhase phase, const OxTerm* terms, i32 term_count, OxSystemRunFn run
) -> u64 {
  ZoneScoped;

  auto desc = ecs_system_desc_t{};
  if (!fill_terms(desc.query.terms, terms, term_count)) {
    return 0;
  }

  memory::ScopedStack stack;
  const auto phase_id = to_phase(phase);
  ecs_id_t phase_ids[] = {ecs_dependson(phase_id), phase_id, 0};
  auto entity_desc = ecs_entity_desc_t{};
  entity_desc.name = name.length ? stack.null_terminate_cstr(to_view(name)) : nullptr;
  entity_desc.add = phase_ids;

  desc.entity = ecs_entity_init(world, &entity_desc);
  desc.callback = system_callback;
  desc.callback_ctx = reinterpret_cast<void*>(run);
  return ecs_system_init(world, &desc);
}

static auto create_query(ecs_world_t* world, const OxTerm* terms, i32 term_count) -> void* {
  ZoneScoped;

  auto desc = ecs_query_desc_t{};
  if (!fill_terms(desc.terms, terms, term_count)) {
    return nullptr;
  }

  desc.cache_kind = EcsQueryCacheAuto;
  return ecs_query_init(world, &desc);
}

static auto destroy_query(void* query) -> void { ecs_query_fini(static_cast<ecs_query_t*>(query)); }

static auto query_iter(void* query, void* iter) -> void {
  const auto* q = static_cast<const ecs_query_t*>(query);
  new (iter) ecs_iter_t(ecs_query_iter(q->world, q));
}

static auto query_next(void* iter, OxTableView* view, void** fields) -> bool {
  auto* it = static_cast<ecs_iter_t*>(iter);
  if (!ecs_query_next(it)) {
    return false;
  }

  fill_view(it, view, fields);
  return true;
}

static auto iter_fini(void* iter) -> void { ecs_iter_fini(static_cast<ecs_iter_t*>(iter)); }

static auto set_behaviour(ecs_world_t* world, u64 entity, OxStringView type_name) -> void {
  ZoneScoped;

  flecs::entity(world, entity).set<ScriptComponent>({.type_name = std::string(to_view(type_name))});
}

auto set_component_db(ecs_world_t* world, ComponentDB* component_db) -> void {
  if (component_db) {
    component_dbs[world] = component_db;
  } else {
    component_dbs.erase(world);
  }
}

auto native_time() -> OxTime& { return frame_time; }

auto set_managed_api(const OxManagedApi* managed_api) -> void { loaded_managed_api = managed_api; }

auto managed_api() -> const OxManagedApi* { return loaded_managed_api; }

auto make_native_api() -> OxNativeApi {
  auto api = OxNativeApi{
    .iter_size = sizeof(ecs_iter_t),
    .child_of = EcsChildOf,
    .is_a = EcsIsA,
    .time = &frame_time,
    .log = native_log,
    .ecs_new = ecs_new,
    .ecs_delete = ecs_delete,
    .ecs_is_alive = ecs_is_alive,
    .ecs_add_id = ecs_add_id,
    .ecs_remove_id = ecs_remove_id,
    .ecs_has_id = ecs_has_id,
    .ecs_get_mut_id = ecs_get_mut_id,
    .ecs_set_id = ecs_set_id,
    .ecs_modified_id = ecs_modified_id,
    .ecs_get_target = ecs_get_target,
    .ecs_enable = ecs_enable,
    .ecs_ensure_id = ecs_ensure_id,
    .ecs_defer_begin = ecs_defer_begin,
    .ecs_defer_end = ecs_defer_end,
    .ecs_defer_suspend = ecs_defer_suspend,
    .ecs_defer_resume = ecs_defer_resume,
    .get_path = get_path,
    .children_iter = children_iter,
    .children_next = children_next,
    .lookup = lookup,
    .get_name = get_name,
    .set_name = set_name,
    .define_component = native_define_component,
    .set_component_defaults = set_component_defaults,
    .bind_components = bind_components,
    .create_system = create_system,
    .create_query = create_query,
    .destroy_query = destroy_query,
    .query_iter = query_iter,
    .query_next = query_next,
    .iter_fini = iter_fini,
    .set_behaviour = set_behaviour,
  };
  fill_input_api(api);
  fill_physics_api(api);
  fill_content_api(api);
  fill_ui_api(api);
  fill_net_api(api);
  fill_app_api(api);
  return api;
}
} // namespace ox
