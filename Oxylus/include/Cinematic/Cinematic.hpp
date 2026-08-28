#pragma once

#include <filesystem>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <string>
#include <vector>

#include "Cinematic/Easing.hpp"
#include "Cinematic/Fwd.hpp"
#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
auto value_kind_name(CinematicValueKind kind) -> std::string_view;
auto value_kind_size(CinematicValueKind kind) -> u32;
auto value_kind_component_count(CinematicValueKind kind) -> u32;
// discrete kinds cannot be blended, so a segment holds the previous key until the next one is reached
auto value_kind_is_discrete(CinematicValueKind kind) -> bool;
auto camera_interp_name(CameraInterp interp) -> std::string_view;

struct CinematicKey {
  f32 time = 0.0f;
  glm::vec4 value = {};
  // easing into this key
  Easing easing = Easing::Linear;
};

struct CinematicPropertyTrack {
  std::string name = {};
  std::string entity_path = {};
  std::string component_path = {};
  std::string member_path = {};
  CinematicValueKind kind = CinematicValueKind::Float;
  std::vector<CinematicKey> keys = {};
  bool enabled = true;
};

struct CameraWaypoint {
  f32 time = 0.0f;
  glm::vec3 position = {};
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  f32 fov = 60.0f;
  Easing easing = Easing::Linear;
  // starts a new shot: the outgoing pose is held right up to this time and the camera then snaps
  // here with no blending, and the spline never reaches across the boundary
  bool cut = false;
};

struct CinematicCameraTrack {
  std::string name = {};
  std::string entity_path = {};
  std::vector<CameraWaypoint> waypoints = {};
  CameraInterp interp = CameraInterp::CatmullRom;
  bool constant_speed = false;
  bool drive_fov = true;
  bool enabled = true;
};

struct Cinematic {
  constexpr static u32 ARC_LUT_SIZE = 256;

  std::string name = {};
  f32 duration = 5.0f;
  bool loop = false;
  std::vector<CinematicCameraTrack> camera_tracks = {};
  std::vector<CinematicPropertyTrack> property_tracks = {};

  Cinematic() = default;
  ~Cinematic() = default;
  Cinematic(const Cinematic&) = delete;
  Cinematic& operator=(const Cinematic&) = delete;
  Cinematic(Cinematic&&) = default;
  Cinematic& operator=(Cinematic&&) = default;

  static auto make_default() -> Cinematic;

  static auto read(const std::filesystem::path& path) -> option<Cinematic>;
  auto write(this const Cinematic& self, const std::filesystem::path& path) -> bool;

  // longest keyed time across every track, so the editor can grow `duration` to fit what was recorded
  auto keyed_extent(this const Cinematic& self) -> f32;
};

namespace cinematic {
auto sample_property(std::span<const CinematicKey> keys, CinematicValueKind kind, f32 time) -> glm::vec4;

// ignores `constant_speed`; `sample_camera` reparameterizes before calling into this
auto sample_camera_raw(const CinematicCameraTrack& track, f32 time) -> CameraWaypoint;
auto sample_camera(const CinematicCameraTrack& track, std::span<const f32> arc_lut, f32 time) -> CameraWaypoint;

auto track_has_cuts(const CinematicCameraTrack& track) -> bool;

// cumulative chord length at `out.size()` evenly spaced times, normalized so the last entry is 1.
// a cut track gets no LUT: arc length cannot describe a path that holds still and then teleports
auto build_arc_length_lut(const CinematicCameraTrack& track, std::span<f32> out) -> void;

// reads a component member into the vec4 representation the keys use
auto read_value(const void* src, CinematicValueKind kind) -> glm::vec4;
// converts back out of the vec4 representation, so Int/Bool/Enum land as their own types
auto write_value(void* dst, CinematicValueKind kind, const glm::vec4& value) -> void;
} // namespace cinematic
} // namespace ox
