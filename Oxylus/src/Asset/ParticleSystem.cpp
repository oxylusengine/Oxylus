#include "Asset/ParticleSystem.hpp"

#include <algorithm>
#include <glm/common.hpp>
#include <glm/gtc/packing.hpp>
#include <initializer_list>
#include <simdjson.h>

#include "OS/File.hpp"
#include "Utils/JsonWriter.hpp"
#include "Utils/Log.hpp"

namespace ox {
auto read_json_f32(simdjson::ondemand::value json, std::string_view key, f32& value) -> void {
  auto result = json[key].get_double();
  if (!result.error()) {
    value = static_cast<f32>(result.value_unsafe());
  }
}

auto read_json_u32(simdjson::ondemand::value json, std::string_view key, u32& value) -> void {
  auto result = json[key].get_uint64();
  if (!result.error()) {
    value = static_cast<u32>(result.value_unsafe());
  }
}

auto read_json_bool(simdjson::ondemand::value json, std::string_view key, bool& value) -> void {
  auto result = json[key].get_bool();
  if (!result.error()) {
    value = result.value_unsafe();
  }
}

auto read_json_uuid(simdjson::ondemand::value json, std::string_view key, UUID& value) -> void {
  auto result = json[key].get_string();
  if (result.error()) {
    return;
  }

  if (auto uuid = UUID::from_string(result.value_unsafe()); uuid.has_value()) {
    value = uuid.value();
  }
}

template <glm::length_t N, typename T>
auto read_json_vec(simdjson::ondemand::value json, std::string_view key, glm::vec<N, T>& value) -> void {
  constexpr static std::string_view components[] = {"x", "y", "z", "w"};
  auto field = json[key];
  if (field.error()) {
    return;
  }

  for (glm::length_t i = 0; i < N; i++) {
    auto result = field[components[i]].get_double();
    if (!result.error()) {
      value[i] = static_cast<T>(result.value_unsafe());
    }
  }
}

template <typename T>
auto read_json_enum(simdjson::ondemand::value json, std::string_view key, T& value) -> void {
  auto result = json[key].get_uint64();
  if (!result.error()) {
    value = static_cast<T>(result.value_unsafe());
  }
}

auto write_particle_graph(JsonWriter& writer, const ParticleGraph& graph) -> void {
  writer.begin_obj();

  writer["nodes"].begin_array();
  for (const auto& node : graph.nodes) {
    writer.begin_obj();
    writer["id"] = std::to_underlying(node.id);
    writer["type"] = std::to_underlying(node.type);
    writer["position"] = node.canvas_position;
    writer["index"] = node.index;
    writer["params"].begin_array();
    for (const auto& param : node.params) {
      writer = param;
    }
    writer.end_array();
    writer.end_obj();
  }
  writer.end_array();

  writer["links"].begin_array();
  for (const auto& link : graph.links) {
    writer.begin_obj();
    writer["id"] = std::to_underlying(link.id);
    writer["from"] = std::to_underlying(link.from_node);
    writer["to"] = std::to_underlying(link.to_node);
    writer["pin"] = link.to_pin;
    writer.end_obj();
  }
  writer.end_array();

  writer.end_obj();
}

auto read_particle_graph(simdjson::ondemand::value json, ParticleGraph& graph) -> void {
  auto nodes_json = json["nodes"];
  if (!nodes_json.error()) {
    for (auto node_json : nodes_json.get_array()) {
      auto node = ParticleNode{};
      auto id = 0_u32;
      auto type = 0_u32;
      read_json_u32(node_json.value_unsafe(), "id", id);
      read_json_u32(node_json.value_unsafe(), "type", type);
      read_json_u32(node_json.value_unsafe(), "index", node.index);
      read_json_vec(node_json.value_unsafe(), "position", node.canvas_position);
      node.id = static_cast<ParticleNodeID>(id);
      node.type = static_cast<ParticleNodeType>(std::min(type, static_cast<u32>(ParticleNodeType::Count) - 1));

      auto params_json = node_json.value_unsafe()["params"];
      if (!params_json.error()) {
        for (auto param_json : params_json.get_array()) {
          auto param = glm::vec4(0.0f);
          constexpr static std::string_view components[] = {"x", "y", "z", "w"};
          for (auto i = 0; i < 4; i++) {
            auto result = param_json.value_unsafe()[components[i]].get_double();
            if (!result.error()) {
              param[i] = static_cast<f32>(result.value_unsafe());
            }
          }
          node.params.push_back(param);
        }
      }

      node.params.resize(particle_node_desc(node.type).param_count, glm::vec4(0.0f));
      graph.nodes.emplace_back(std::move(node));
    }
  }

  auto links_json = json["links"];
  if (!links_json.error()) {
    for (auto link_json : links_json.get_array()) {
      auto link = ParticleLink{};
      auto id = 0_u32;
      auto from = 0_u32;
      auto to = 0_u32;
      read_json_u32(link_json.value_unsafe(), "id", id);
      read_json_u32(link_json.value_unsafe(), "from", from);
      read_json_u32(link_json.value_unsafe(), "to", to);
      read_json_u32(link_json.value_unsafe(), "pin", link.to_pin);
      link.id = static_cast<ParticleLinkID>(id);
      link.from_node = static_cast<ParticleNodeID>(from);
      link.to_node = static_cast<ParticleNodeID>(to);
      graph.links.emplace_back(link);
    }
  }

  const auto link_count = graph.links.size();
  std::erase_if(graph.links, [&graph](const ParticleLink& link) {
    const auto* target = graph.find_node(link.to_node);
    return target == nullptr || graph.find_node(link.from_node) == nullptr ||
           link.to_pin >= particle_node_desc(target->type).input_count;
  });

  if (graph.links.size() != link_count) {
    OX_LOG_WARN("Dropped {} malformed particle graph link(s).", link_count - graph.links.size());
  }
}

auto ParticleCurve::sample(this const ParticleCurve& self, const f32 t) -> f32 {
  if (self.points.empty()) {
    return 0.0f;
  }

  if (t <= self.points.front().x) {
    return self.points.front().y;
  }

  for (usize i = 1; i < self.points.size(); i++) {
    const auto& previous = self.points[i - 1];
    const auto& current = self.points[i];
    if (t <= current.x) {
      const auto span = current.x - previous.x;
      const auto factor = span > 0.0f ? (t - previous.x) / span : 0.0f;
      return glm::mix(previous.y, current.y, factor);
    }
  }

  return self.points.back().y;
}

auto ParticleGradient::sample(this const ParticleGradient& self, const f32 t) -> glm::vec4 {
  if (self.keys.empty()) {
    return glm::vec4(1.0f);
  }

  if (t <= self.keys.front().t) {
    return self.keys.front().color;
  }

  for (usize i = 1; i < self.keys.size(); i++) {
    const auto& previous = self.keys[i - 1];
    const auto& current = self.keys[i];
    if (t <= current.t) {
      const auto span = current.t - previous.t;
      const auto factor = span > 0.0f ? (t - previous.t) / span : 0.0f;
      return glm::mix(previous.color, current.color, factor);
    }
  }

  return self.keys.back().color;
}

auto ParticleSystem::make_default() -> ParticleSystem {
  auto system = ParticleSystem{};

  system.curves.emplace_back(ParticleCurve{.name = "Size", .points = {{0.0f, 1.0f}, {1.0f, 0.0f}}});
  system.gradients.emplace_back(ParticleGradient{});

  const auto set_params = [](ParticleGraph& graph, ParticleNodeID id, std::initializer_list<glm::vec4> params) {
    for (auto& node : graph.nodes) {
      if (node.id == id) {
        node.params.assign(params.begin(), params.end());
        return;
      }
    }
  };

  auto& spawn = system.spawn_graph;
  const auto lifetime = spawn.add_node(ParticleNodeType::Random, {-460.0f, -140.0f});
  const auto direction = spawn.add_node(ParticleNodeType::ReadVelocity, {-460.0f, 20.0f});
  const auto speed = spawn.add_node(ParticleNodeType::Random, {-460.0f, 160.0f});
  const auto velocity = spawn.add_node(ParticleNodeType::Multiply, {-220.0f, 60.0f});
  const auto size = spawn.add_node(ParticleNodeType::Constant, {-220.0f, 260.0f});
  const auto set_lifetime = spawn.add_node(ParticleNodeType::SetLifetime, {40.0f, -140.0f});
  const auto set_velocity = spawn.add_node(ParticleNodeType::SetVelocity, {40.0f, 60.0f});
  const auto set_size = spawn.add_node(ParticleNodeType::SetSize, {40.0f, 260.0f});

  set_params(spawn, lifetime, {glm::vec4(1.0f), glm::vec4(2.0f)});
  set_params(spawn, speed, {glm::vec4(1.5f), glm::vec4(3.0f)});
  set_params(spawn, size, {glm::vec4(0.25f, 0.25f, 0.0f, 0.0f)});

  spawn.add_link(lifetime, set_lifetime, 0);
  spawn.add_link(direction, velocity, 0);
  spawn.add_link(speed, velocity, 1);
  spawn.add_link(velocity, set_velocity, 0);
  spawn.add_link(size, set_size, 0);

  auto& update = system.update_graph;
  const auto gravity = update.add_node(ParticleNodeType::Constant, {-460.0f, -140.0f});
  const auto delta = update.add_node(ParticleNodeType::ReadDeltaTime, {-460.0f, 0.0f});
  const auto gravity_step = update.add_node(ParticleNodeType::Multiply, {-220.0f, -80.0f});
  const auto add_velocity = update.add_node(ParticleNodeType::AddVelocity, {40.0f, -80.0f});
  const auto age = update.add_node(ParticleNodeType::ReadAge, {-460.0f, 180.0f});
  const auto color = update.add_node(ParticleNodeType::Gradient, {-220.0f, 140.0f});
  const auto set_color = update.add_node(ParticleNodeType::SetColor, {40.0f, 140.0f});
  const auto curve = update.add_node(ParticleNodeType::Curve, {-460.0f, 320.0f});
  const auto base_size = update.add_node(ParticleNodeType::Constant, {-460.0f, 440.0f});
  const auto scaled_size = update.add_node(ParticleNodeType::Multiply, {-220.0f, 360.0f});
  const auto set_size_over_life = update.add_node(ParticleNodeType::SetSize, {40.0f, 360.0f});

  set_params(update, gravity, {glm::vec4(0.0f, -9.81f, 0.0f, 0.0f)});
  set_params(update, base_size, {glm::vec4(0.25f, 0.25f, 0.0f, 0.0f)});

  update.add_link(gravity, gravity_step, 0);
  update.add_link(delta, gravity_step, 1);
  update.add_link(gravity_step, add_velocity, 0);
  update.add_link(age, color, 0);
  update.add_link(color, set_color, 0);
  update.add_link(age, curve, 0);
  // The update graph's SetSize replaces rather than scales, so the spawn size is reapplied here.
  update.add_link(curve, scaled_size, 0);
  update.add_link(base_size, scaled_size, 1);
  update.add_link(scaled_size, set_size_over_life, 0);

  system.recompile();

  return system;
}

auto ParticleSystem::recompile(this ParticleSystem& self) -> void {
  ZoneScoped;

  auto compiled = compile_particle_graphs(self.emitter_graph, self.spawn_graph, self.update_graph);
  if (compiled) {
    self.programs = std::move(compiled.value());
    self.compile_error.clear();
  } else {
    self.programs = {};
    self.compile_error = compiled.error();
    OX_LOG_ERROR("Particle system failed to compile: {}", self.compile_error);
  }

  const auto row_count = self.atlas_row_count();
  if (row_count == 0) {
    return;
  }

  // Sampling walks the control points in order, so the bake works off sorted copies -- the authored
  // order stays as the editor left it, and dragging a point past its neighbour cannot corrupt it.
  auto curves = self.curves;
  for (auto& curve : curves) {
    std::ranges::sort(curve.points, [](const glm::vec2& a, const glm::vec2& b) { return a.x < b.x; });
  }

  auto gradients = self.gradients;
  for (auto& gradient : gradients) {
    std::ranges::sort(gradient.keys, [](const ParticleGradientKey& a, const ParticleGradientKey& b) {
      return a.t < b.t;
    });
  }

  auto texels = std::vector<glm::u16vec4>(static_cast<usize>(CURVE_ATLAS_WIDTH) * row_count);
  for (auto row = 0_u32; row < row_count; row++) {
    for (auto x = 0_u32; x < CURVE_ATLAS_WIDTH; x++) {
      const auto t = static_cast<f32>(x) / static_cast<f32>(CURVE_ATLAS_WIDTH - 1);
      const auto value = row < curves.size() ? glm::vec4(curves[row].sample(t))
                                             : gradients[row - curves.size()].sample(t);
      texels[row * CURVE_ATLAS_WIDTH + x] = glm::u16vec4(
        glm::packHalf1x16(value.x),
        glm::packHalf1x16(value.y),
        glm::packHalf1x16(value.z),
        glm::packHalf1x16(value.w)
      );
    }
  }

  if (self.curve_atlas && self.curve_atlas.get_extent().height != row_count) {
    self.curve_atlas.destroy();
    self.curve_atlas = {};
  }

  if (!self.curve_atlas) {
    self.curve_atlas = Texture::create({
      .format = vuk::Format::eR16G16B16A16Sfloat,
      .extent = {CURVE_ATLAS_WIDTH, row_count, 1},
      .usage = vuk::ImageUsageFlagBits::eSampled | vuk::ImageUsageFlagBits::eTransferDst,
      .sampler_info = {
        .magFilter = vuk::Filter::eLinear,
        .minFilter = vuk::Filter::eLinear,
        .mipmapMode = vuk::SamplerMipmapMode::eNearest,
        .addressModeU = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vuk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vuk::SamplerAddressMode::eClampToEdge
      },
    });
  }

  self.curve_atlas.upload(
    std::span(reinterpret_cast<const u8*>(texels.data()), texels.size() * sizeof(glm::u16vec4)),
    vuk::eFragmentSampled
  );
}

auto ParticleSystem::destroy(this ParticleSystem& self) -> void {
  if (self.curve_atlas) {
    self.curve_atlas.destroy();
    self.curve_atlas = {};
  }
}

auto ParticleSystem::find_parameter(this const ParticleSystem& self, const std::string_view name) -> option<u32> {
  for (usize i = 0; i < self.parameters.size(); i++) {
    if (self.parameters[i].name == name) {
      return static_cast<u32>(i);
    }
  }

  return nullopt;
}

auto ParticleSystem::write(this const ParticleSystem& self, const std::filesystem::path& path) -> bool {
  ZoneScoped;

  JsonWriter writer{};
  writer.begin_obj();

  writer["emitter"].begin_obj();
  writer["capacity"] = self.emitter.capacity;
  writer["spawn_rate"] = self.emitter.spawn_rate;
  writer["duration"] = self.emitter.duration;
  writer["start_delay"] = self.emitter.start_delay;
  writer["looping"] = self.emitter.looping;
  writer["lifetime"] = self.emitter.lifetime;
  writer["shape"] = std::to_underlying(self.emitter.shape);
  writer["shape_size"] = self.emitter.shape_size;
  writer["shape_angle"] = self.emitter.shape_angle;
  writer["simulation_space"] = std::to_underlying(self.emitter.simulation_space);
  writer["seed"] = self.emitter.seed;
  writer.end_obj();

  writer["render"].begin_obj();
  writer["material"] = self.render.material.str().c_str();
  writer["mesh"] = self.render.mesh.str().c_str();
  writer["render_mode"] = std::to_underlying(self.render.render_mode);
  writer["billboard"] = std::to_underlying(self.render.billboard);
  writer["blend"] = std::to_underlying(self.render.blend);
  writer["flipbook"] = self.render.flipbook;
  writer["soft_particle_distance"] = self.render.soft_particle_distance;
  writer["velocity_stretch"] = self.render.velocity_stretch;
  writer["restitution"] = self.render.restitution;
  writer["sort"] = self.render.sort;
  writer["depth_collision"] = self.render.depth_collision;
  writer.end_obj();

  writer["curves"].begin_array();
  for (const auto& curve : self.curves) {
    writer.begin_obj();
    writer["name"] = curve.name;
    writer["points"].begin_array();
    for (const auto& point : curve.points) {
      writer = point;
    }
    writer.end_array();
    writer.end_obj();
  }
  writer.end_array();

  writer["gradients"].begin_array();
  for (const auto& gradient : self.gradients) {
    writer.begin_obj();
    writer["name"] = gradient.name;
    writer["keys"].begin_array();
    for (const auto& key : gradient.keys) {
      writer.begin_obj();
      writer["t"] = key.t;
      writer["color"] = key.color;
      writer.end_obj();
    }
    writer.end_array();
    writer.end_obj();
  }
  writer.end_array();

  writer["parameters"].begin_array();
  for (const auto& parameter : self.parameters) {
    writer.begin_obj();
    writer["name"] = parameter.name;
    writer["default"] = parameter.default_value;
    writer.end_obj();
  }
  writer.end_array();

  writer["emitter_graph"];
  write_particle_graph(writer, self.emitter_graph);
  writer["spawn_graph"];
  write_particle_graph(writer, self.spawn_graph);
  writer["update_graph"];
  write_particle_graph(writer, self.update_graph);

  writer.end_obj();

  auto file = File(path, FileAccess::Write);
  if (!file) {
    return false;
  }

  file.write(writer.stream.view());
  file.close();

  return true;
}

auto ParticleSystem::read(const std::filesystem::path& path) -> option<ParticleSystem> {
  ZoneScoped;

  auto contents = File::to_string(path);
  if (contents.empty()) {
    OX_LOG_ERROR("Failed to read particle system '{}'.", path);
    return nullopt;
  }

  auto padded = simdjson::padded_string(contents);
  simdjson::ondemand::parser parser;
  auto doc = parser.iterate(padded);
  if (doc.error()) {
    OX_LOG_ERROR("Failed to parse particle system '{}': {}", path, simdjson::error_message(doc.error()));
    return nullopt;
  }

  auto system = ParticleSystem{};

  if (auto emitter_json = doc["emitter"]; !emitter_json.error()) {
    auto value = emitter_json.value_unsafe();
    read_json_u32(value, "capacity", system.emitter.capacity);
    read_json_f32(value, "spawn_rate", system.emitter.spawn_rate);
    read_json_f32(value, "duration", system.emitter.duration);
    read_json_f32(value, "start_delay", system.emitter.start_delay);
    read_json_bool(value, "looping", system.emitter.looping);
    read_json_vec(value, "lifetime", system.emitter.lifetime);
    read_json_enum(value, "shape", system.emitter.shape);
    read_json_vec(value, "shape_size", system.emitter.shape_size);
    read_json_f32(value, "shape_angle", system.emitter.shape_angle);
    read_json_enum(value, "simulation_space", system.emitter.simulation_space);
    read_json_u32(value, "seed", system.emitter.seed);
  }

  if (auto render_json = doc["render"]; !render_json.error()) {
    auto value = render_json.value_unsafe();
    read_json_uuid(value, "material", system.render.material);
    read_json_uuid(value, "mesh", system.render.mesh);
    read_json_enum(value, "render_mode", system.render.render_mode);
    read_json_enum(value, "billboard", system.render.billboard);
    read_json_enum(value, "blend", system.render.blend);
    read_json_vec(value, "flipbook", system.render.flipbook);
    read_json_f32(value, "soft_particle_distance", system.render.soft_particle_distance);
    read_json_f32(value, "velocity_stretch", system.render.velocity_stretch);
    read_json_f32(value, "restitution", system.render.restitution);
    read_json_bool(value, "sort", system.render.sort);
    read_json_bool(value, "depth_collision", system.render.depth_collision);
  }

  if (auto curves_json = doc["curves"]; !curves_json.error()) {
    for (auto curve_json : curves_json.get_array()) {
      auto curve = ParticleCurve{};
      curve.points.clear();
      if (auto name = curve_json.value_unsafe()["name"].get_string(); !name.error()) {
        curve.name = name.value_unsafe();
      }
      if (auto points_json = curve_json.value_unsafe()["points"]; !points_json.error()) {
        for (auto point_json : points_json.get_array()) {
          auto point = glm::vec2(0.0f);
          if (auto x = point_json.value_unsafe()["x"].get_double(); !x.error()) {
            point.x = static_cast<f32>(x.value_unsafe());
          }
          if (auto y = point_json.value_unsafe()["y"].get_double(); !y.error()) {
            point.y = static_cast<f32>(y.value_unsafe());
          }
          curve.points.push_back(point);
        }
      }
      system.curves.emplace_back(std::move(curve));
    }
  }

  if (auto gradients_json = doc["gradients"]; !gradients_json.error()) {
    for (auto gradient_json : gradients_json.get_array()) {
      auto gradient = ParticleGradient{};
      gradient.keys.clear();
      if (auto name = gradient_json.value_unsafe()["name"].get_string(); !name.error()) {
        gradient.name = name.value_unsafe();
      }
      if (auto keys_json = gradient_json.value_unsafe()["keys"]; !keys_json.error()) {
        for (auto key_json : keys_json.get_array()) {
          auto key = ParticleGradientKey{};
          read_json_f32(key_json.value_unsafe(), "t", key.t);
          read_json_vec(key_json.value_unsafe(), "color", key.color);
          gradient.keys.push_back(key);
        }
      }
      system.gradients.emplace_back(std::move(gradient));
    }
  }

  if (auto parameters_json = doc["parameters"]; !parameters_json.error()) {
    for (auto parameter_json : parameters_json.get_array()) {
      auto parameter = ParticleParameter{};
      if (auto name = parameter_json.value_unsafe()["name"].get_string(); !name.error()) {
        parameter.name = name.value_unsafe();
      }
      read_json_vec(parameter_json.value_unsafe(), "default", parameter.default_value);
      system.parameters.emplace_back(std::move(parameter));
    }
  }

  if (auto graph_json = doc["emitter_graph"]; !graph_json.error()) {
    read_particle_graph(graph_json.value_unsafe(), system.emitter_graph);
  }

  if (auto graph_json = doc["spawn_graph"]; !graph_json.error()) {
    read_particle_graph(graph_json.value_unsafe(), system.spawn_graph);
  }

  if (auto graph_json = doc["update_graph"]; !graph_json.error()) {
    read_particle_graph(graph_json.value_unsafe(), system.update_graph);
  }

  system.recompile();

  return system;
}
} // namespace ox
