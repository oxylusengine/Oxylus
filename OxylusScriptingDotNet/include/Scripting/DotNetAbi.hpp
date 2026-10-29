#pragma once

#include "Core/Types.hpp"

// mirrored field for field by managed/Oxylus.Scripting/Interop/Abi.cs, bump the version on any change to either side
#define OX_DOTNET_ABI_VERSION 13u

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
  SceneBehaviour = 1,
  EntityBehaviour = 2,
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

struct OxVec2 {
  f32 x = 0.0f;
  f32 y = 0.0f;
};

struct OxVec3 {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
};

struct OxVec4 {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 z = 0.0f;
  f32 w = 0.0f;
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

struct OxTerrainWaypoint {
  OxVec3 position = {};
  f32 width = 0.0f;
  f32 shoulder = 0.0f;
};

struct OxTerrainStamp {
  OxVec3 start = {};
  OxVec3 end = {};
  f32 radius = 0.0f;
  f32 height = 0.0f;
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
  AddLinearVelocity = 8,
  // `position` is the world space point it acts at
  AddForceAtPosition = 9,
  AddImpulseAtPosition = 10,
  // `position`, `rotation` and `delta_time`
  MoveKinematic = 11,
  SetPosition = 12,
  SetRotation = 13,
  SetPositionAndRotation = 14,
};

// what a command reads out of it depends on the command, the rest is ignored
struct OxBodyArgs {
  OxVec3 vector = {};
  f32 delta_time = 0.0f;
  OxVec3 position = {};
  u32 padding = 0;
  OxQuat rotation = {};
};

// `value` of physics_body_set is an f32 for all of them: a JPH::EMotionType, a layer index, or a bool as 0 or 1
enum class OxBodyProperty : i32 {
  MotionType = 0,
  ObjectLayer = 1,
  Friction = 2,
  Restitution = 3,
  IsSensor = 4,
  CollideKinematicVsNonDynamic = 5,
  UseManifoldReduction = 6,
  ApplyGyroscopicForce = 7,
  EnhancedInternalEdgeRemoval = 8,
};

enum class OxBodyFlag : u32 {
  IsSensor = 1u << 0,
  CollideKinematicVsNonDynamic = 1u << 1,
  UseManifoldReduction = 1u << 2,
  ApplyGyroscopicForce = 1u << 3,
  EnhancedInternalEdgeRemoval = 1u << 4,
};

struct OxBodyState {
  OxQuat rotation = {};
  OxVec3 position = {};
  u32 is_active = 0;
  OxVec3 linear_velocity = {};
  // a JPH::EMotionType: static, kinematic, dynamic
  u32 motion_type = 0;
  OxVec3 angular_velocity = {};
  u32 object_layer = 0;
  OxVec3 center_of_mass = {};
  f32 friction = 0.0f;
  OxVec3 bounds_min = {};
  f32 restitution = 0.0f;
  OxVec3 bounds_max = {};
  // OxBodyFlag bits
  u32 flags = 0;
  // zero unless the body is dynamic
  OxVec3 accumulated_force = {};
  u32 padding0 = 0;
  OxVec3 accumulated_torque = {};
  u32 padding1 = 0;
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

struct OxVehicleState {
  f32 engine_rpm = 0.0f;
  f32 engine_angular_velocity = 0.0f;
  i32 gear = 0;
  // m/s along the chassis forward axis, negative while rolling backwards
  f32 forward_speed = 0.0f;
};

struct OxWheelState {
  u32 is_contacting = 0;
  // both zero while airborne
  f32 longitudinal_slip = 0.0f;
  f32 lateral_slip = 0.0f;
};

// an asset's UUID, the 16 bytes UUID keeps without the debug string it carries in debug builds
struct OxAssetId {
  u64 low = 0;
  u64 high = 0;
};

struct OxAssetInfo {
  // an AssetType
  u32 type = 0;
  u32 is_loaded = 0;
  u64 ref_count = 0;
};

// Asset/Material.hpp's Material. `alpha_mode` and `sampling_mode` are its enums' values, `flip_normal_y` is 0 or 1
struct OxMaterialData {
  OxVec4 albedo_color = {1.0f, 1.0f, 1.0f, 1.0f};
  OxVec2 uv_size = {1.0f, 1.0f};
  OxVec2 uv_offset = {};
  OxVec3 emissive_color = {};
  f32 roughness_factor = 1.0f;
  f32 metallic_factor = 0.0f;
  f32 normal_scale = 1.0f;
  f32 occlusion_strength = 1.0f;
  u32 alpha_mode = 0;
  f32 alpha_cutoff = 0.1f;
  u32 sampling_mode = 0;
  u32 flip_normal_y = 0;
  u32 padding = 0;
  OxAssetId albedo_texture = {};
  OxAssetId normal_texture = {};
  OxAssetId emissive_texture = {};
  OxAssetId metallic_roughness_texture = {};
  OxAssetId occlusion_texture = {};
};

enum class OxParticleCommand : i32 {
  Play = 0,
  Stop = 1,
  Restart = 2,
  // `value` is the particle count
  EmitBurst = 3,
};

enum class OxCinematicCommand : i32 {
  Play = 0,
  Stop = 1,
  // `value` is the time in seconds
  Seek = 2,
};

struct OxCinematicState {
  f32 time = 0.0f;
  f32 duration = 0.0f;
};

// zero while the app has no window
struct OxWindowSize {
  i32 pixels_x = 0;
  i32 pixels_y = 0;
  i32 logical_x = 0;
  i32 logical_y = 0;
};

enum class OxVfsDir : i32 {
  App = 0,
  Assets = 1,
  Cooked = 2,
};

enum class OxAudioCommand : i32 {
  Play = 0,
  Stop = 1,
  Pause = 2,
  Resume = 3,
};

struct OxAudioState {
  u32 is_playing = 0;
  f32 cursor = 0.0f;
  f32 length = 0.0f;
  u32 padding = 0;
};

// one value crossing for a data model variable, a data event argument or an RPC parameter. `text` holds a string's
// UTF-8, a UUID's 16 bytes or a byte array, valid for the call it is handed to
enum class OxValueType : u32 {
  None = 0,
  Bool = 1,
  U8 = 2,
  U16 = 3,
  I32 = 4,
  I64 = 5,
  F32 = 6,
  F64 = 7,
  String = 8,
  Uuid = 9,
  Bytes = 10,
};

struct OxValue {
  OxValueType type = OxValueType::None;
  u32 padding = 0;
  i64 integer = 0;
  f64 number = 0.0;
  OxStringView text = {};
};

enum class OxUiDocumentCommand : i32 {
  Show = 0,
  Hide = 1,
  Close = 2,
};

struct OxUiEvent {
  OxStringView type = {};
  u64 target = 0;
  u64 current = 0;
  f32 mouse_x = 0.0f;
  f32 mouse_y = 0.0f;
};

enum class OxNetEvent : i32 {
  // on a server, `peer` is the client
  ClientConnected = 0,
  ClientDisconnected = 1,
  // on a client, `peer` is the net id the server gave it
  Connected = 2,
  // on a client, `reason` is the NetClientStatus it ended in
  Disconnected = 3,
  // on a server, `peer` is the client and `reason` the sequence it acknowledged
  ClientAcked = 4,
};

struct OxNetClientState {
  // a NetClientStatus
  u32 status = 0;
  u32 ping = 0;
  u64 net_id = 0;
  u32 rtt = 0;
  u32 packets_lost = 0;
  u32 sent_bytes = 0;
  u32 received_bytes = 0;
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
  u64 is_a = 0;
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
  // adds the component when the entity lacks it, which runs its ctor hook, and returns the storage even while deferred
  auto (*ecs_ensure_id)(ecs_world_t* world, u64 entity, u64 id, usize size) -> void* = nullptr;
  auto (*ecs_defer_begin)(ecs_world_t* world) -> bool = nullptr;
  auto (*ecs_defer_end)(ecs_world_t* world) -> bool = nullptr;
  auto (*ecs_defer_suspend)(ecs_world_t* world) -> void = nullptr;
  auto (*ecs_defer_resume)(ecs_world_t* world) -> void = nullptr;
  // a "." separated path, writes up to `capacity` bytes and returns the full length so a short buffer can be retried
  auto (*get_path)(ecs_world_t* world, u64 entity, c8* out, u32 capacity) -> u32 = nullptr;
  // iterates the entity's children into `iter`, which is `iter_size` bytes. Finish it with iter_fini unless
  // children_next ran it out. The entities of a result are valid until the next call
  auto (*children_iter)(ecs_world_t* world, u64 parent, void* iter) -> void = nullptr;
  auto (*children_next)(void* iter, const u64** entities, i32* count) -> bool = nullptr;

  auto (*lookup)(ecs_world_t* world, OxStringView path) -> u64 = nullptr;
  auto (*get_name)(ecs_world_t* world, u64 entity, OxStringView* name) -> void = nullptr;
  auto (*set_name)(ecs_world_t* world, u64 entity, OxStringView name) -> void = nullptr;

  auto (*define_component)(ecs_world_t* world, OxStringView path, const OxComponentField* fields, i32 field_count)
    -> u64 = nullptr;
  // makes `component` start from `size` bytes of `defaults` wherever flecs creates it. The hook owns a copy
  auto (*set_component_defaults)(ecs_world_t* world, u64 component, const void* defaults, u32 size)
    -> OxStatus = nullptr;
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
  // InvalidArgument for what jolt would reject: a velocity on a static body, a force on a body that isn't dynamic
  auto (*physics_body_command)(ecs_world_t* world, u64 entity, OxBodyCommand command, const OxBodyArgs* args)
    -> OxStatus = nullptr;
  // InvalidArgument for a value jolt would reject, or a motion type on a body that was never kinematic or dynamic
  auto (*physics_body_set)(ecs_world_t* world, u64 entity, OxBodyProperty property, f32 value) -> OxStatus = nullptr;
  auto (*physics_character_state)(ecs_world_t* world, u64 entity, OxCharacterState* state) -> OxStatus = nullptr;
  auto (*physics_character_command)(ecs_world_t* world, u64 entity, OxCharacterCommand command, const OxVec3* value)
    -> OxStatus = nullptr;
  // NotFound until the scene starts and creates the vehicle's constraint
  auto (*physics_vehicle_state)(ecs_world_t* world, u64 entity, OxVehicleState* state) -> OxStatus = nullptr;
  // the entity is a child of the vehicle
  auto (*physics_wheel_state)(ecs_world_t* world, u64 entity, OxWheelState* state) -> OxStatus = nullptr;
  // from the entity's CameraComponent as the scene's camera_update left it, NotFound without one. A camera that hasn't
  // been updated yet gives a zero ray. Only `origin` and `direction` are written
  auto (*physics_screen_ray)(
    ecs_world_t* world, u64 entity, const OxVec2* screen_position, const OxVec2* screen_size, OxRay* ray
  ) -> OxStatus = nullptr;

  // `path` is relative to ASSETS_DIR, the way gameplay names content. Acquiring loads the asset when it isn't yet and
  // takes one reference, which one release gives back
  auto (*asset_find)(OxStringView path, OxAssetId* id) -> bool = nullptr;
  auto (*asset_acquire)(const OxAssetId* id) -> bool = nullptr;
  auto (*asset_release)(const OxAssetId* id) -> void = nullptr;
  auto (*asset_info)(const OxAssetId* id, OxAssetInfo* info) -> bool = nullptr;
  // a loaded model's materials. Fills up to `capacity` and writes how many there are to `count`, so a short buffer can
  // be retried. NotFound for anything but a loaded model
  auto (*asset_materials)(const OxAssetId* model, OxAssetId* materials, i32 capacity, i32* count) -> OxStatus = nullptr;
  // NotFound for anything but a loaded material
  auto (*asset_material_get)(const OxAssetId* material, OxMaterialData* data) -> OxStatus = nullptr;
  // marks the material dirty for the renderer. A texture it names is loaded and referenced as many times as the
  // material is held, and the one it no longer names is given those references back. InvalidArgument for an enum value
  // out of range or a texture slot that names something else
  auto (*asset_material_set)(const OxAssetId* material, const OxMaterialData* data) -> OxStatus = nullptr;

  // the entity's AudioSourceComponent. A sound belongs to its asset, so entities sharing one share its playback
  auto (*audio_command)(ecs_world_t* world, u64 entity, OxAudioCommand command) -> OxStatus = nullptr;
  auto (*audio_state)(ecs_world_t* world, u64 entity, OxAudioState* state) -> OxStatus = nullptr;
  // acquires `sound` for the component and gives the reference its previous sound held back
  auto (*audio_set_sound)(ecs_world_t* world, u64 entity, const OxAssetId* sound) -> OxStatus = nullptr;
  // a loaded audio asset, a null position plays it flat
  auto (*audio_one_shot)(const OxAssetId* sound, f32 volume, const OxVec3* position) -> OxStatus = nullptr;

  // a scene's entities, with the TransformComponent and LayerComponent every scene entity has, 0 on failure
  auto (*scene_spawn)(ecs_world_t* world, OxStringView name, u64 parent) -> u64 = nullptr;
  auto (*scene_spawn_model)(ecs_world_t* world, const OxAssetId* model, u64 parent) -> u64 = nullptr;
  auto (*scene_spawn_particles)(ecs_world_t* world, const OxAssetId* particle_system) -> u64 = nullptr;
  // each does nothing for an entity without an emitter, a cinematic player or a scene
  auto (*scene_particles_command)(ecs_world_t* world, u64 entity, OxParticleCommand command, u32 value)
    -> void = nullptr;
  auto (*scene_particles_playing)(ecs_world_t* world, u64 entity) -> bool = nullptr;
  auto (*scene_particles_set_parameter)(ecs_world_t* world, u64 entity, u32 index, const OxVec4* value)
    -> void = nullptr;
  // false when the emitter's asset has no parameter by that name
  auto (*scene_particles_set_parameter_named)(ecs_world_t* world, u64 entity, OxStringView name, const OxVec4* value)
    -> bool = nullptr;
  auto (*scene_cinematic_command)(ecs_world_t* world, u64 entity, OxCinematicCommand command, f32 value)
    -> void = nullptr;
  auto (*scene_cinematic_state)(ecs_world_t* world, u64 entity, OxCinematicState* state) -> void = nullptr;
  auto (*scene_input_focused)(ecs_world_t* world) -> bool = nullptr;
  // false for a world without a scene
  auto (*scene_uuid)(ecs_world_t* world, OxAssetId* id) -> bool = nullptr;
  // writes the scene's editor JSON to a physical path
  auto (*scene_save)(ecs_world_t* world, OxStringView path) -> OxStatus = nullptr;
  auto (*scene_set_ui_dpi_ratio)(ecs_world_t* world, f32 ratio) -> void = nullptr;
  auto (*scene_clear_ui_dpi_ratio)(ecs_world_t* world) -> void = nullptr;
  // emits SceneLoadRequestEvent, the scene's owner does the load
  auto (*scene_request_load)(const OxAssetId* scene) -> OxStatus = nullptr;
  // position.y is ignored. A count of 0 clears the track. NotFound for a world without a scene, InvalidArgument for a
  // count of 1 or past TERRAIN_TRACK_MAX_POINTS, a non-finite value, a width <= 0, or a negative shoulder or smoothing
  auto (*scene_set_terrain_track)(ecs_world_t* world, const OxTerrainWaypoint* waypoints, i32 count, f32 smoothing)
    -> OxStatus = nullptr;
  // the y components are ignored. A count of 0 clears the stamps. NotFound for a world without a scene, InvalidArgument
  // for a count past TERRAIN_STAMP_MAX, a non-finite value, or a radius <= 0. A negative height presses a trench
  auto (*scene_set_terrain_stamps)(ecs_world_t* world, const OxTerrainStamp* stamps, i32 count) -> OxStatus = nullptr;

  // each writes up to `capacity` bytes of UTF-8 and returns the full length, so a short buffer can be retried
  auto (*vfs_resolve)(OxVfsDir dir, OxStringView relative_path, c8* out, u32 capacity) -> u32 = nullptr;
  auto (*vfs_to_physical)(OxStringView path, c8* out, u32 capacity) -> u32 = nullptr;
  auto (*vfs_to_virtual)(OxStringView path, c8* out, u32 capacity) -> u32 = nullptr;

  // RmlUi in the world's scene context. Elements are handles that go stale, never dangle, once RmlUi drops the
  // element, and a stale one returns NotFound. `path` is relative to ASSETS_DIR
  auto (*ui_load_document)(ecs_world_t* world, OxStringView path, u64* document) -> OxStatus = nullptr;
  // `path` is relative to ASSETS_DIR
  auto (*ui_load_font_face)(OxStringView path, bool is_fallback) -> OxStatus = nullptr;
  auto (*ui_document)(u64 document, OxUiDocumentCommand command) -> OxStatus = nullptr;
  auto (*ui_query)(u64 element, OxStringView selector, u64* found) -> OxStatus = nullptr;
  auto (*ui_set_inner_rml)(u64 element, OxStringView rml) -> OxStatus = nullptr;
  // writes up to `capacity` bytes and the full length to `length`, so a short buffer can be retried
  auto (*ui_get_inner_rml)(u64 element, c8* out, u32 capacity, u32* length) -> OxStatus = nullptr;
  auto (*ui_set_attribute)(u64 element, OxStringView name, OxStringView value) -> OxStatus = nullptr;
  auto (*ui_get_attribute)(u64 element, OxStringView name, c8* out, u32 capacity, u32* length) -> OxStatus = nullptr;
  auto (*ui_set_property)(u64 element, OxStringView name, OxStringView value) -> OxStatus = nullptr;
  auto (*ui_set_class)(u64 element, OxStringView name, bool is_active) -> OxStatus = nullptr;
  // each event reaches OxManagedApi::ui_event with `listener`
  auto (*ui_add_listener)(u64 element, OxStringView event, u64* listener) -> OxStatus = nullptr;
  auto (*ui_remove_listener)(u64 listener) -> void = nullptr;
  // a variable's reads and writes reach ui_model_get and ui_model_set, an event callback ui_model_event, each with
  // the `binding` it was bound with
  auto (*ui_model_create)(ecs_world_t* world, OxStringView name, u64* model) -> OxStatus = nullptr;
  auto (*ui_model_bind)(u64 model, OxStringView name, i32 binding, bool is_writable) -> OxStatus = nullptr;
  auto (*ui_model_bind_event)(u64 model, OxStringView name, i32 binding) -> OxStatus = nullptr;
  // an empty name dirties every variable
  auto (*ui_model_dirty)(u64 model, OxStringView name) -> void = nullptr;
  auto (*ui_model_remove)(u64 model) -> void = nullptr;

  auto (*ui_clear_style_sheet_cache)() -> void = nullptr;
  auto (*ui_clear_template_cache)() -> void = nullptr;

  auto (*window_size)(OxWindowSize* size) -> void = nullptr;
  // the app's frame limit in milliseconds, negative for none. All three do nothing without an App
  auto (*time_max_frame_time)() -> f64 = nullptr;
  auto (*time_set_max_frame_time)(f64 millis) -> void = nullptr;
  auto (*time_reset_max_frame_time)() -> void = nullptr;
  // stops the app after the current frame, does nothing without an App
  auto (*app_quit)() -> void = nullptr;

  // servers and clients the scripts open, ticked by DotNetScripting::update and closed when the scripts unload
  auto (*net_server_start)(u16 port, u32 max_clients, u64* server) -> OxStatus = nullptr;
  auto (*net_client_connect)(OxStringView host, u16 port, u32 timeout_ms, u64* client) -> OxStatus = nullptr;
  auto (*net_close)(u64 endpoint) -> void = nullptr;
  auto (*net_client_state)(u64 client, OxNetClientState* state) -> OxStatus = nullptr;
  auto (*net_server_clients)(u64 server, u64* clients, i32 capacity) -> i32 = nullptr;
  // a call reaches OxManagedApi::net_rpc with `binding`
  auto (*net_register_rpc)(u64 endpoint, OxStringView name, i32 binding) -> OxStatus = nullptr;
  // from a server to `client`, or to every client when it is ~0. A client's call goes to its server
  auto (*net_call)(u64 endpoint, u64 client, OxStringView name, const OxValue* args, i32 count, bool is_reliable)
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
  // a scene behaviour instance, `handle` names it from then on. Its systems register in `world` with it
  auto (*script_create)(ecs_world_t* world, OxStringView type_name, u64* handle) -> OxStatus = nullptr;
  auto (*script_destroy)(u64 handle) -> void = nullptr;
  // replaces whatever behaviour the entity had
  auto (*behaviour_add)(ecs_world_t* world, u64 entity, OxStringView type_name) -> OxStatus = nullptr;
  auto (*behaviour_remove)(ecs_world_t* world, u64 entity) -> void = nullptr;

  // each hook reaches every scene behaviour and then every entity behaviour in the world, in one call
  auto (*world_start)(ecs_world_t* world) -> void = nullptr;
  auto (*world_stop)(ecs_world_t* world) -> void = nullptr;
  auto (*world_update)(ecs_world_t* world, f32 delta_time) -> void = nullptr;
  auto (*world_fixed_update)(ecs_world_t* world, f32 delta_time) -> void = nullptr;
  auto (*world_render)(ecs_world_t* world, u32 width, u32 height) -> void = nullptr;
  auto (*world_contacts)(ecs_world_t* world, const PhysicsContact* contacts, i32 count) -> void = nullptr;
  auto (*world_body_activation)(ecs_world_t* world, u64 entity, bool is_activated) -> void = nullptr;

  auto (*ui_event)(u64 listener, const OxUiEvent* event) -> void = nullptr;
  auto (*ui_model_get)(u64 model, i32 binding, OxValue* value) -> void = nullptr;
  auto (*ui_model_set)(u64 model, i32 binding, const OxValue* value) -> void = nullptr;
  auto (*ui_model_event)(u64 model, i32 binding, const OxUiEvent* event, const OxValue* args, i32 count)
    -> void = nullptr;
  auto (*net_rpc)(u64 endpoint, i32 binding, u64 sender, const OxValue* args, i32 count) -> void = nullptr;
  auto (*net_event)(u64 endpoint, OxNetEvent event, u64 peer, u32 reason) -> void = nullptr;
  // a client got a scene snapshot. The entity ids are valid for the call
  auto (*net_snapshot)(
    u64 endpoint, u8 sequence, const u64* changed, i32 changed_count, const u64* removed, i32 removed_count
  ) -> void = nullptr;
  auto (*window_resized)(u32 width, u32 height) -> void = nullptr;
};

using OxManagedInitFn = auto (*)(const OxNativeApi* native_api, OxManagedApi* managed_api, u32 abi_version) -> OxStatus;
} // namespace ox
