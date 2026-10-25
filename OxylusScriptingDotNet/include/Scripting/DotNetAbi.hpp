#pragma once

#include "Core/Types.hpp"

// mirrored field for field by managed/Oxylus.Scripting/Interop/Abi.cs, bump the version on any change to either side
#define OX_DOTNET_ABI_VERSION 5u

struct ecs_world_t;

namespace ox {
struct PhysicsContact;
class SceneScriptHost;

enum class OxStatus : i32 {
  Ok = 0,
  AbiMismatch = 1,
  Exception = 2,
  NotFound = 3,
  LayoutMismatch = 4,
  InvalidArgument = 5,
};

enum class OxLogLevel : i32 {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
};

enum class OxPhase : i32 {
  OnLoad = 0,
  PostLoad = 1,
  PreUpdate = 2,
  OnUpdate = 3,
  OnValidate = 4,
  PostUpdate = 5,
  PreStore = 6,
  OnStore = 7,
};

enum class OxTermAccess : i32 {
  InOut = 0,
  In = 1,
  Out = 2,
};

enum class OxScriptKind : i32 {
  None = 0,
  SceneScript = 1,
  Behaviour = 2,
};

// UTF-8, not null terminated
struct OxStringView {
  const c8* data = nullptr;
  usize length = 0;
};

// `name` points into managed memory that stays put until the scripts unload
struct OxScriptTypeInfo {
  OxStringView name = {};
  OxScriptKind kind = OxScriptKind::None;
};

enum class OxInputState : i32 {
  // this frame only
  Pressed = 0,
  Released = 1,
  Held = 2,
};

enum class OxInputContextOp : i32 {
  Set = 0,
  Push = 1,
  Pop = 2,
};

// Core/Input.hpp's InputCode, flattened: `type` is an InputType, `code` the ScanCode, MouseCode, MouseAxisCode,
// GamepadButtonCode or GamepadAxisCode it selects, and `axis_direction` an InputCode::AxisDirection
struct OxInputCode {
  u8 type = 0;
  u8 axis_direction = 0;
  u16 mod = 0;
  i32 code = 0;
};

struct OxMouseState {
  f32 position_x = 0.0f;
  f32 position_y = 0.0f;
  f32 delta_x = 0.0f;
  f32 delta_y = 0.0f;
  f32 scroll = 0.0f;
  u32 is_moved = 0;
};

struct OxVec3 {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
};

struct OxQuat {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
  f32 w = 1.0f;
};

struct OxRay {
  OxVec3 origin = {};
  f32 max_distance = 0.0f;
  OxVec3 direction = {};
  // one bit per object layer, PhysicsLayers in Physics/PhysicsInterfaces.hpp
  u32 layer_mask = ~0_u32;
  u64 ignore_entity = 0;
};

struct OxRayHit {
  u64 entity = 0;
  OxVec3 point = {};
  f32 distance = 0.0f;
  OxVec3 normal = {};
  u32 padding = 0;
};

enum class OxBodyCommand : i32 {
  SetLinearVelocity = 0,
  SetAngularVelocity = 1,
  AddForce = 2,
  AddImpulse = 3,
  AddTorque = 4,
  AddAngularImpulse = 5,
  Activate = 6,
  Deactivate = 7,
};

struct OxBodyState {
  OxQuat rotation = {};
  OxVec3 position = {};
  u32 is_active = 0;
  OxVec3 linear_velocity = {};
  OxVec3 angular_velocity = {};
};

enum class OxCharacterCommand : i32 {
  SetLinearVelocity = 0,
  AddLinearVelocity = 1,
  AddImpulse = 2,
  SetPosition = 3,
};

struct OxCharacterState {
  OxVec3 position = {};
  // JPH::CharacterBase::EGroundState: on ground, on steep ground, not supported, in air
  u32 ground_state = 0;
  OxVec3 linear_velocity = {};
  OxVec3 ground_normal = {};
};

// the app's frame clock, written once a frame by DotNetScripting::update, so reading it from C# is a load
struct OxTime {
  f64 elapsed = 0.0;
  f32 delta_time = 0.0f;
  u64 frame = 0;
};

struct OxTerm {
  u64 id = 0;
  OxTermAccess access = OxTermAccess::InOut;
};

// one matched table, every field owned by it, so `fields[i]` points at `count` values
struct OxTableView {
  ecs_world_t* world = nullptr;
  u64 system = 0;
  const u64* entities = nullptr;
  void* const* fields = nullptr;
  i32 count = 0;
  f32 delta_time = 0.0f;
};

struct OxMemberLayout {
  OxStringView name = {};
  u32 offset = 0;
  u32 size = 0;
};

struct OxComponentLayout {
  OxStringView path = {};
  const OxMemberLayout* members = nullptr;
  u32 member_count = 0;
  u32 size = 0;
  u32 alignment = 0;
};

// matches ComponentFieldType in Scene/Scene.hpp
struct OxComponentField {
  OxStringView name = {};
  i32 type = 0;
};

using OxSystemRunFn = auto (*)(const OxTableView* view) -> void;

struct OxNativeApi {
  u32 size = sizeof(OxNativeApi);
  // the bytes a query iterator needs, managed code owns that storage
  u32 iter_size = 0;
  u64 child_of = 0;
  const OxTime* time = nullptr;

  auto (*log)(OxLogLevel level, const c8* message, usize length) -> void = nullptr;

  // raw flecs, no wrapper in between
  auto (*ecs_new)(ecs_world_t* world) -> u64 = nullptr;
  auto (*ecs_delete)(ecs_world_t* world, u64 entity) -> void = nullptr;
  auto (*ecs_is_alive)(const ecs_world_t* world, u64 entity) -> bool = nullptr;
  auto (*ecs_add_id)(ecs_world_t* world, u64 entity, u64 id) -> void = nullptr;
  auto (*ecs_remove_id)(ecs_world_t* world, u64 entity, u64 id) -> void = nullptr;
  auto (*ecs_has_id)(const ecs_world_t* world, u64 entity, u64 id) -> bool = nullptr;
  auto (*ecs_get_mut_id)(const ecs_world_t* world, u64 entity, u64 id) -> void* = nullptr;
  auto (*ecs_set_id)(ecs_world_t* world, u64 entity, u64 id, usize size, const void* value) -> void = nullptr;
  auto (*ecs_modified_id)(ecs_world_t* world, u64 entity, u64 id) -> void = nullptr;
  auto (*ecs_get_target)(const ecs_world_t* world, u64 entity, u64 relationship, i32 index) -> u64 = nullptr;
  auto (*ecs_enable)(ecs_world_t* world, u64 entity, bool enabled) -> void = nullptr;

  auto (*lookup)(ecs_world_t* world, OxStringView path) -> u64 = nullptr;
  auto (*get_name)(ecs_world_t* world, u64 entity, OxStringView* name) -> void = nullptr;
  auto (*set_name)(ecs_world_t* world, u64 entity, OxStringView name) -> void = nullptr;

  auto (*define_component)(ecs_world_t* world, OxStringView path, const OxComponentField* fields, i32 field_count)
    -> u64 = nullptr;
  // resolves every layout's component in this world into `ids` and checks it against flecs meta
  auto (*bind_components)(ecs_world_t* world, const OxComponentLayout* layouts, i32 layout_count, u64* ids)
    -> OxStatus = nullptr;

  auto (*create_system)(
    ecs_world_t* world, OxStringView name, OxPhase phase, const OxTerm* terms, i32 term_count, OxSystemRunFn run
  ) -> u64 = nullptr;
  auto (*create_query)(ecs_world_t* world, const OxTerm* terms, i32 term_count) -> void* = nullptr;
  auto (*destroy_query)(void* query) -> void = nullptr;
  auto (*query_iter)(void* query, void* iter) -> void = nullptr;
  // `fields` is caller storage for one pointer per term, `view->fields` points at it
  auto (*query_next)(void* iter, OxTableView* view, void** fields) -> bool = nullptr;
  auto (*iter_fini)(void* iter) -> void = nullptr;

  // sets the entity's ScriptComponent, which is what creates the behaviour while the scene runs
  auto (*set_behaviour)(ecs_world_t* world, u64 entity, OxStringView type_name) -> void = nullptr;

  // Core/Input.hpp, all of them no-ops returning nothing set while the app has no Input module
  auto (*input_key)(u32 scan_code, OxInputState state) -> bool = nullptr;
  auto (*input_mouse_button)(u32 mouse_code, OxInputState state) -> bool = nullptr;
  auto (*input_mouse)(OxMouseState* state) -> void = nullptr;
  auto (*input_modifiers)() -> u16 = nullptr;
  auto (*input_gamepad_button)(u32 gamepad, i32 button, OxInputState state) -> bool = nullptr;
  auto (*input_gamepad_axis)(u32 gamepad, i32 axis) -> f32 = nullptr;
  // the connected gamepads' ids, fills up to `capacity` and returns how many there are
  auto (*input_gamepads)(u32* gamepads, i32 capacity) -> i32 = nullptr;
  auto (*input_action)(OxStringView action, u32 gamepad, OxInputState state) -> bool = nullptr;
  auto (*input_action_axis)(OxStringView action, u32 gamepad, f32* axis) -> void = nullptr;
  auto (*input_bind_action)(OxStringView action, OxStringView context, const OxInputCode* inputs, i32 input_count)
    -> OxStatus = nullptr;
  auto (*input_unbind_action)(OxStringView action) -> OxStatus = nullptr;
  auto (*input_context)(OxInputContextOp op, OxStringView context) -> void = nullptr;
  // window coordinates, and both do nothing without a window
  auto (*input_warp_mouse)(f32 x, f32 y) -> void = nullptr;
  auto (*input_set_relative_mouse)(bool is_enabled) -> void = nullptr;

  // a scene's physics, NotFound for an entity without a live body, which is any entity before runtime_start
  auto (*physics_raycast)(ecs_world_t* world, const OxRay* ray, OxRayHit* hit) -> bool = nullptr;
  auto (*physics_body_state)(ecs_world_t* world, u64 entity, OxBodyState* state) -> OxStatus = nullptr;
  auto (*physics_body_command)(ecs_world_t* world, u64 entity, OxBodyCommand command, const OxVec3* value)
    -> OxStatus = nullptr;
  auto (*physics_character_state)(ecs_world_t* world, u64 entity, OxCharacterState* state) -> OxStatus = nullptr;
  auto (*physics_character_command)(ecs_world_t* world, u64 entity, OxCharacterCommand command, const OxVec3* value)
    -> OxStatus = nullptr;
};

struct OxManagedApi {
  u32 size = sizeof(OxManagedApi);
  auto (*shutdown)() -> OxStatus = nullptr;
  auto (*attach_world)(ecs_world_t* world) -> OxStatus = nullptr;
  auto (*detach_world)(ecs_world_t* world) -> OxStatus = nullptr;
  auto (*allocated_bytes)() -> u64 = nullptr;

  auto (*find_script_type)(OxStringView type_name) -> OxScriptKind = nullptr;
  // fills up to `capacity` entries and returns how many types there are
  auto (*list_script_types)(OxScriptTypeInfo* types, i32 capacity) -> i32 = nullptr;
  // a scene script instance, `handle` names it from then on. Its systems register in `world` with it
  auto (*script_create)(ecs_world_t* world, OxStringView type_name, u64* handle) -> OxStatus = nullptr;
  auto (*script_destroy)(u64 handle) -> void = nullptr;
  // replaces whatever behaviour the entity had
  auto (*behaviour_add)(ecs_world_t* world, u64 entity, OxStringView type_name) -> OxStatus = nullptr;
  auto (*behaviour_remove)(ecs_world_t* world, u64 entity) -> void = nullptr;

  // each hook reaches every scene script and then every behaviour in the world, in one call
  auto (*world_start)(ecs_world_t* world) -> void = nullptr;
  auto (*world_stop)(ecs_world_t* world) -> void = nullptr;
  auto (*world_update)(ecs_world_t* world, f32 delta_time) -> void = nullptr;
  auto (*world_fixed_update)(ecs_world_t* world, f32 delta_time) -> void = nullptr;
  auto (*world_render)(ecs_world_t* world, u32 width, u32 height) -> void = nullptr;
  auto (*world_contacts)(ecs_world_t* world, const PhysicsContact* contacts, i32 count) -> void = nullptr;
  auto (*world_body_activation)(ecs_world_t* world, u64 entity, bool is_activated) -> void = nullptr;
};

using OxManagedInitFn = auto (*)(const OxNativeApi* native_api, OxManagedApi* managed_api, u32 abi_version) -> OxStatus;
} // namespace ox
