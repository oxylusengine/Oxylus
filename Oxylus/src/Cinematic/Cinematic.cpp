#include "Cinematic/Cinematic.hpp"

#include <algorithm>
#include <cstring>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <simdjson.h>

#include "OS/File.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"

namespace ox {
static auto read_f32(simdjson::ondemand::value json, std::string_view key, f32& value) -> void {
  auto result = json[key].get_double();
  if (!result.error()) {
    value = static_cast<f32>(result.value_unsafe());
  }
}

static auto read_bool(simdjson::ondemand::value json, std::string_view key, bool& value) -> void {
  auto result = json[key].get_bool();
  if (!result.error()) {
    value = result.value_unsafe();
  }
}

static auto read_string(simdjson::ondemand::value json, std::string_view key, std::string& value) -> void {
  auto result = json[key].get_string();
  if (!result.error()) {
    value = result.value_unsafe();
  }
}

template <typename T>
static auto read_enum(simdjson::ondemand::value json, std::string_view key, T& value) -> void {
  auto result = json[key].get_uint64();
  if (result.error()) {
    return;
  }

  const auto raw = result.value_unsafe();
  if (raw < static_cast<u64>(T::Count)) {
    value = static_cast<T>(raw);
  }
}

template <glm::length_t N>
static auto read_vec(simdjson::ondemand::value json, std::string_view key, glm::vec<N, f32>& value) -> void {
  constexpr static std::string_view components[] = {"x", "y", "z", "w"};
  auto field = json[key];
  if (field.error()) {
    return;
  }

  for (glm::length_t i = 0; i < N; i++) {
    auto result = field[components[i]].get_double();
    if (!result.error()) {
      value[i] = static_cast<f32>(result.value_unsafe());
    }
  }
}

static auto read_quat(simdjson::ondemand::value json, std::string_view key, glm::quat& value) -> void {
  auto packed = glm::vec4(value.x, value.y, value.z, value.w);
  read_vec(json, key, packed);
  value = glm::quat::wxyz(packed.w, packed.x, packed.y, packed.z);
}

auto value_kind_name(const CinematicValueKind kind) -> std::string_view {
  switch (kind) {
    case CinematicValueKind::Float : return "Float";
    case CinematicValueKind::Float2: return "Float2";
    case CinematicValueKind::Float3: return "Float3";
    case CinematicValueKind::Float4: return "Float4";
    case CinematicValueKind::Quat  : return "Quat";
    case CinematicValueKind::Int   : return "Int";
    case CinematicValueKind::Bool  : return "Bool";
    case CinematicValueKind::Enum  : return "Enum";
    case CinematicValueKind::Count : return {};
  }

  return {};
}

auto value_kind_size(const CinematicValueKind kind) -> u32 {
  switch (kind) {
    case CinematicValueKind::Float : return sizeof(f32);
    case CinematicValueKind::Float2: return sizeof(glm::vec2);
    case CinematicValueKind::Float3: return sizeof(glm::vec3);
    case CinematicValueKind::Float4: return sizeof(glm::vec4);
    case CinematicValueKind::Quat  : return sizeof(glm::quat);
    case CinematicValueKind::Int   : return sizeof(i32);
    case CinematicValueKind::Bool  : return sizeof(bool);
    case CinematicValueKind::Enum  : return sizeof(i32);
    case CinematicValueKind::Count : return 0;
  }

  return 0;
}

auto value_kind_component_count(const CinematicValueKind kind) -> u32 {
  switch (kind) {
    case CinematicValueKind::Float2: return 2;
    case CinematicValueKind::Float3: return 3;
    case CinematicValueKind::Float4:
    case CinematicValueKind::Quat  : return 4;
    default                        : return 1;
  }
}

auto value_kind_is_discrete(const CinematicValueKind kind) -> bool {
  switch (kind) {
    case CinematicValueKind::Int :
    case CinematicValueKind::Bool:
    case CinematicValueKind::Enum: return true;
    default                      : return false;
  }
}

auto camera_interp_name(const CameraInterp interp) -> std::string_view {
  switch (interp) {
    case CameraInterp::Linear    : return "Linear";
    case CameraInterp::CatmullRom: return "Catmull-Rom";
    case CameraInterp::Count     : return {};
  }

  return {};
}

auto Cinematic::make_default() -> Cinematic {
  auto cinematic = Cinematic{};
  cinematic.name = "Cinematic";

  return cinematic;
}

auto Cinematic::keyed_extent(this const Cinematic& self) -> f32 {
  auto extent = 0.0f;
  for (const auto& track : self.camera_tracks) {
    if (!track.waypoints.empty()) {
      extent = glm::max(extent, track.waypoints.back().time);
    }
  }
  for (const auto& track : self.property_tracks) {
    if (!track.keys.empty()) {
      extent = glm::max(extent, track.keys.back().time);
    }
  }

  return extent;
}

auto Cinematic::write(this const Cinematic& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  JsonWriter writer{};
  writer.begin_obj();

  writer["name"] = self.name;
  writer["duration"] = self.duration;
  writer["loop"] = self.loop;

  writer["camera_tracks"].begin_array();
  for (const auto& track : self.camera_tracks) {
    writer.begin_obj();
    writer["name"] = track.name;
    writer["entity_path"] = track.entity_path;
    writer["interp"] = static_cast<u32>(track.interp);
    writer["constant_speed"] = track.constant_speed;
    writer["drive_fov"] = track.drive_fov;
    writer["enabled"] = track.enabled;
    writer["waypoints"].begin_array();
    for (const auto& waypoint : track.waypoints) {
      writer.begin_obj();
      writer["time"] = waypoint.time;
      writer["position"] = waypoint.position;
      writer["rotation"] = waypoint.rotation;
      writer["fov"] = waypoint.fov;
      writer["easing"] = static_cast<u32>(waypoint.easing);
      writer.end_obj();
    }
    writer.end_array();
    writer.end_obj();
  }
  writer.end_array();

  writer["property_tracks"].begin_array();
  for (const auto& track : self.property_tracks) {
    writer.begin_obj();
    writer["name"] = track.name;
    writer["entity_path"] = track.entity_path;
    writer["component_path"] = track.component_path;
    writer["member_path"] = track.member_path;
    writer["kind"] = static_cast<u32>(track.kind);
    writer["enabled"] = track.enabled;
    writer["keys"].begin_array();
    for (const auto& key : track.keys) {
      writer.begin_obj();
      writer["time"] = key.time;
      writer["value"] = key.value;
      writer["easing"] = static_cast<u32>(key.easing);
      writer.end_obj();
    }
    writer.end_array();
    writer.end_obj();
  }
  writer.end_array();

  writer.end_obj();

  auto file = File(path, FileAccess::Write);
  if (!file) {
    return false;
  }

  file.write(writer.stream.view());
  file.close();

  return true;
}

auto Cinematic::read(const std::filesystem::path& path) -> option<Cinematic> {
  ZoneScoped;

  auto contents = File::to_string(path);
  if (contents.empty()) {
    OX_LOG_ERROR("Failed to read cinematic '{}'.", path);
    return nullopt;
  }

  auto padded = simdjson::padded_string(contents);
  simdjson::ondemand::parser parser;
  auto doc = parser.iterate(padded);
  if (doc.error()) {
    OX_LOG_ERROR("Failed to parse cinematic '{}': {}", path, simdjson::error_message(doc.error()));
    return nullopt;
  }

  auto cinematic = Cinematic{};

  if (auto name = doc["name"].get_string(); !name.error()) {
    cinematic.name = name.value_unsafe();
  }
  if (auto duration = doc["duration"].get_double(); !duration.error()) {
    cinematic.duration = static_cast<f32>(duration.value_unsafe());
  }
  if (auto loop = doc["loop"].get_bool(); !loop.error()) {
    cinematic.loop = loop.value_unsafe();
  }

  if (auto tracks_json = doc["camera_tracks"]; !tracks_json.error()) {
    for (auto track_json : tracks_json.get_array()) {
      auto value = track_json.value_unsafe();
      auto track = CinematicCameraTrack{};
      read_string(value, "name", track.name);
      read_string(value, "entity_path", track.entity_path);
      read_enum(value, "interp", track.interp);
      read_bool(value, "constant_speed", track.constant_speed);
      read_bool(value, "drive_fov", track.drive_fov);
      read_bool(value, "enabled", track.enabled);

      if (auto waypoints_json = value["waypoints"]; !waypoints_json.error()) {
        for (auto waypoint_json : waypoints_json.get_array()) {
          auto waypoint_value = waypoint_json.value_unsafe();
          auto waypoint = CameraWaypoint{};
          read_f32(waypoint_value, "time", waypoint.time);
          read_vec(waypoint_value, "position", waypoint.position);
          read_quat(waypoint_value, "rotation", waypoint.rotation);
          read_f32(waypoint_value, "fov", waypoint.fov);
          read_enum(waypoint_value, "easing", waypoint.easing);
          track.waypoints.emplace_back(waypoint);
        }
      }

      cinematic.camera_tracks.emplace_back(std::move(track));
    }
  }

  if (auto tracks_json = doc["property_tracks"]; !tracks_json.error()) {
    for (auto track_json : tracks_json.get_array()) {
      auto value = track_json.value_unsafe();
      auto track = CinematicPropertyTrack{};
      read_string(value, "name", track.name);
      read_string(value, "entity_path", track.entity_path);
      read_string(value, "component_path", track.component_path);
      read_string(value, "member_path", track.member_path);
      read_enum(value, "kind", track.kind);
      read_bool(value, "enabled", track.enabled);

      if (auto keys_json = value["keys"]; !keys_json.error()) {
        for (auto key_json : keys_json.get_array()) {
          auto key_value = key_json.value_unsafe();
          auto key = CinematicKey{};
          read_f32(key_value, "time", key.time);
          read_vec(key_value, "value", key.value);
          read_enum(key_value, "easing", key.easing);
          track.keys.emplace_back(key);
        }
      }

      cinematic.property_tracks.emplace_back(std::move(track));
    }
  }

  return cinematic;
}

namespace cinematic {
static auto catmull_rom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, const f32 t)
  -> glm::vec3 {
  const auto t2 = t * t;
  const auto t3 = t2 * t;

  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

auto sample_property(std::span<const CinematicKey> keys, const CinematicValueKind kind, const f32 time) -> glm::vec4 {
  ZoneScoped;

  if (keys.empty()) {
    return glm::vec4(0.0f);
  }

  if (time <= keys.front().time) {
    return keys.front().value;
  }

  if (time >= keys.back().time) {
    return keys.back().value;
  }

  const auto next = std::ranges::lower_bound(keys, time, {}, &CinematicKey::time);
  const auto index = static_cast<usize>(next - keys.begin());
  const auto& previous = keys[index - 1];
  const auto& current = keys[index];

  const auto span = current.time - previous.time;
  const auto factor = span > 0.0f ? (time - previous.time) / span : 1.0f;

  if (value_kind_is_discrete(kind)) {
    return factor >= 1.0f ? current.value : previous.value;
  }

  const auto t = ease(current.easing, factor);

  if (kind == CinematicValueKind::Quat) {
    const auto from = glm::quat::wxyz(previous.value.w, previous.value.x, previous.value.y, previous.value.z);
    const auto to = glm::quat::wxyz(current.value.w, current.value.x, current.value.y, current.value.z);
    const auto result = glm::normalize(glm::slerp(glm::normalize(from), glm::normalize(to), t));
    return {result.x, result.y, result.z, result.w};
  }

  return glm::mix(previous.value, current.value, t);
}

auto sample_camera_raw(const CinematicCameraTrack& track, const f32 time) -> CameraWaypoint {
  ZoneScoped;

  if (track.waypoints.empty()) {
    return {};
  }

  if (track.waypoints.size() == 1 || time <= track.waypoints.front().time) {
    auto result = track.waypoints.front();
    result.time = time;
    return result;
  }

  if (time >= track.waypoints.back().time) {
    auto result = track.waypoints.back();
    result.time = time;
    return result;
  }

  const auto next = std::ranges::lower_bound(track.waypoints, time, {}, &CameraWaypoint::time);
  const auto index = static_cast<usize>(next - track.waypoints.begin());
  const auto& previous = track.waypoints[index - 1];
  const auto& current = track.waypoints[index];

  const auto span = current.time - previous.time;
  const auto factor = span > 0.0f ? (time - previous.time) / span : 1.0f;
  const auto t = ease(current.easing, factor);

  auto result = CameraWaypoint{};
  result.time = time;
  result.easing = current.easing;

  if (track.interp == CameraInterp::CatmullRom) {
    // the missing neighbour at each end is extrapolated, not duplicated: repeating the endpoint bends
    // an evenly spaced straight run into an ease-in
    const auto before = index >= 2 ? track.waypoints[index - 2].position : 2.0f * previous.position - current.position;
    const auto after = index + 1 < track.waypoints.size() ? track.waypoints[index + 1].position
                                                          : 2.0f * current.position - previous.position;
    result.position = catmull_rom(before, previous.position, current.position, after, t);
  } else {
    result.position = glm::mix(previous.position, current.position, t);
  }

  result.rotation = glm::normalize(glm::slerp(glm::normalize(previous.rotation), glm::normalize(current.rotation), t));
  result.fov = glm::mix(previous.fov, current.fov, t);

  return result;
}

auto build_arc_length_lut(const CinematicCameraTrack& track, std::span<f32> out) -> void {
  ZoneScoped;

  if (out.empty()) {
    return;
  }

  std::ranges::fill(out, 0.0f);

  if (track.waypoints.size() < 2) {
    return;
  }

  const auto start = track.waypoints.front().time;
  const auto end = track.waypoints.back().time;
  const auto range = end - start;
  if (range <= 0.0f) {
    return;
  }

  const auto step = 1.0f / static_cast<f32>(out.size() - 1);
  auto previous = sample_camera_raw(track, start).position;
  auto total = 0.0f;
  for (usize i = 1; i < out.size(); i++) {
    const auto current = sample_camera_raw(track, start + range * step * static_cast<f32>(i)).position;
    total += glm::distance(previous, current);
    out[i] = total;
    previous = current;
  }

  if (total <= 0.0f) {
    return;
  }

  for (auto& entry : out) {
    entry /= total;
  }
}

auto sample_camera(const CinematicCameraTrack& track, std::span<const f32> arc_lut, const f32 time) -> CameraWaypoint {
  ZoneScoped;

  if (!track.constant_speed || arc_lut.size() < 2 || track.waypoints.size() < 2) {
    return sample_camera_raw(track, time);
  }

  const auto start = track.waypoints.front().time;
  const auto end = track.waypoints.back().time;
  const auto range = end - start;
  if (range <= 0.0f) {
    return sample_camera_raw(track, time);
  }

  const auto distance = glm::clamp((time - start) / range, 0.0f, 1.0f);
  const auto next = std::ranges::lower_bound(arc_lut, distance);
  if (next == arc_lut.end()) {
    return sample_camera_raw(track, end);
  }

  const auto index = static_cast<usize>(next - arc_lut.begin());
  auto fractional = static_cast<f32>(index);
  if (index > 0) {
    const auto span = arc_lut[index] - arc_lut[index - 1];
    const auto local = span > 0.0f ? (distance - arc_lut[index - 1]) / span : 0.0f;
    fractional = static_cast<f32>(index - 1) + local;
  }

  const auto remapped = start + range * (fractional / static_cast<f32>(arc_lut.size() - 1));

  return sample_camera_raw(track, remapped);
}

auto read_value(const void* src, const CinematicValueKind kind) -> glm::vec4 {
  auto value = glm::vec4(0.0f);

  switch (kind) {
    case CinematicValueKind::Float: {
      auto scalar = 0.0f;
      std::memcpy(&scalar, src, sizeof(scalar));
      value.x = scalar;
    } break;
    case CinematicValueKind::Float2: {
      auto vec = glm::vec2(0.0f);
      std::memcpy(&vec, src, sizeof(vec));
      value = {vec.x, vec.y, 0.0f, 0.0f};
    } break;
    case CinematicValueKind::Float3: {
      auto vec = glm::vec3(0.0f);
      std::memcpy(&vec, src, sizeof(vec));
      value = {vec.x, vec.y, vec.z, 0.0f};
    } break;
    case CinematicValueKind::Float4: {
      std::memcpy(&value, src, sizeof(value));
    } break;
    case CinematicValueKind::Quat: {
      auto quaternion = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
      std::memcpy(&quaternion, src, sizeof(quaternion));
      value = {quaternion.x, quaternion.y, quaternion.z, quaternion.w};
    } break;
    case CinematicValueKind::Int :
    case CinematicValueKind::Enum: {
      auto integer = 0_i32;
      std::memcpy(&integer, src, sizeof(integer));
      value.x = static_cast<f32>(integer);
    } break;
    case CinematicValueKind::Bool: {
      auto boolean = false;
      std::memcpy(&boolean, src, sizeof(boolean));
      value.x = boolean ? 1.0f : 0.0f;
    } break;
    case CinematicValueKind::Count: break;
  }

  return value;
}

auto write_value(void* dst, const CinematicValueKind kind, const glm::vec4& value) -> void {
  switch (kind) {
    case CinematicValueKind::Float: {
      const auto scalar = value.x;
      std::memcpy(dst, &scalar, sizeof(scalar));
    } break;
    case CinematicValueKind::Float2: {
      const auto vec = glm::vec2(value);
      std::memcpy(dst, &vec, sizeof(vec));
    } break;
    case CinematicValueKind::Float3: {
      const auto vec = glm::vec3(value);
      std::memcpy(dst, &vec, sizeof(vec));
    } break;
    case CinematicValueKind::Float4: {
      std::memcpy(dst, &value, sizeof(value));
    } break;
    case CinematicValueKind::Quat: {
      const auto quaternion = glm::normalize(glm::quat::wxyz(value.w, value.x, value.y, value.z));
      std::memcpy(dst, &quaternion, sizeof(quaternion));
    } break;
    case CinematicValueKind::Int :
    case CinematicValueKind::Enum: {
      const auto integer = static_cast<i32>(glm::round(value.x));
      std::memcpy(dst, &integer, sizeof(integer));
    } break;
    case CinematicValueKind::Bool: {
      const auto boolean = value.x != 0.0f;
      std::memcpy(dst, &boolean, sizeof(boolean));
    } break;
    case CinematicValueKind::Count: break;
  }
}
} // namespace cinematic
} // namespace ox
