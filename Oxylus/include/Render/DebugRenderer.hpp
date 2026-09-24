#pragma once

#include <array>
#include <expected>
#include <glm/ext/quaternion_float.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "Core/Types.hpp"
#include "Physics/RayCast.hpp"
#include "Render/BoundingVolume.hpp"

namespace ox {
class Renderer;

class DebugRenderer {
public:
  constexpr static auto MODULE_NAME = "DebugRenderer";
  using module_dependencies = std::tuple<Renderer>;

  struct Vertex {
    glm::vec3 position = {};
    // rgba8, r in the lowest byte (same layout as JPH::Color)
    u32 color = 0;
  };

  enum class Primitive : u8 { Lines, Triangles };

  // camera the last flush was built for, text faces it and physics picks geometry lods from it
  struct View {
    glm::vec3 position = {};
    glm::vec3 right = {1.0f, 0.0f, 0.0f};
    glm::vec3 up = {0.0f, 1.0f, 0.0f};
  };

  struct VertexRange {
    u32 offset = 0;
    u32 count = 0;
  };

  // indexed by depth_tested
  struct DrawRanges {
    std::array<VertexRange, 2> lines = {};
    std::array<VertexRange, 2> triangles = {};

    auto empty(this const DrawRanges& self) -> bool {
      return self.lines[0].count + self.lines[1].count + self.triangles[0].count + self.triangles[1].count == 0;
    }
  };

  auto init(this DebugRenderer& self) -> std::expected<void, std::string>;
  auto deinit(this DebugRenderer& self) -> std::expected<void, std::string>;

  static auto pack_color(const glm::vec4& color) -> u32;

  // drawn as a small three axis cross
  auto draw_point(
    this DebugRenderer& self,
    const glm::vec3& pos,
    f32 point_radius,
    const glm::vec4& color = glm::vec4(1.0f),
    bool depth_tested = false
  ) -> void;
  auto draw_line(
    this DebugRenderer& self,
    const glm::vec3& start,
    const glm::vec3& end,
    f32 line_width,
    const glm::vec4& color = glm::vec4(1.0f),
    bool depth_tested = false
  ) -> void;
  // solid and back face culled, counter clockwise is front
  auto draw_triangle(
    this DebugRenderer& self,
    const glm::vec3& v0,
    const glm::vec3& v1,
    const glm::vec3& v2,
    const glm::vec4& color,
    bool depth_tested = false
  ) -> void;
  // camera facing stroke text centered on position, height is the cap height in world units
  auto draw_text(
    this DebugRenderer& self,
    const glm::vec3& position,
    std::string_view text,
    f32 height,
    const glm::vec4& color = glm::vec4(1.0f),
    bool depth_tested = false
  ) -> void;
  auto draw_circle(
    this DebugRenderer& self,
    i32 num_verts,
    f32 radius,
    const glm::vec3& position,
    const glm::quat& rotation,
    const glm::vec4& color,
    bool depth_tested = false
  ) -> void;
  auto draw_sphere(
    this DebugRenderer& self, f32 radius, const glm::vec3& position, const glm::vec4& color, bool depth_tested = false
  ) -> void;
  auto draw_capsule(
    this DebugRenderer& self,
    const glm::vec3& position,
    const glm::quat& rotation,
    f32 height,
    f32 radius,
    const glm::vec4& color,
    bool depth_tested = false
  ) -> void;
  auto draw_cone(
    this DebugRenderer& self,
    i32 num_circle_verts,
    i32 num_lines_to_circle,
    f32 angle,
    f32 length,
    const glm::vec3& position,
    const glm::quat& rotation,
    const glm::vec4& color,
    bool depth_tested = false
  ) -> void;
  auto draw_aabb(
    this DebugRenderer& self,
    const AABB& aabb,
    const glm::vec4& color = glm::vec4(1.0f),
    bool corners_only = false,
    f32 width = 1.0f,
    bool depth_tested = false
  ) -> void;
  auto draw_frustum(this DebugRenderer& self, const glm::mat4& frustum, const glm::vec4& color, f32 near, f32 far)
    -> void;
  auto draw_ray(
    this DebugRenderer& self, const RayCast& ray, const glm::vec4& color, f32 distance, bool depth_tested = false
  ) -> void;

  // bulk path: reserves vertex_count vertices (2 per line, 3 per triangle) under one lock and lets fill write them.
  // thread safe, jolt draws from its job threads during the physics step
  template <typename F>
  auto emit(this DebugRenderer& self, Primitive primitive, usize vertex_count, bool depth_tested, F&& fill) -> void {
    if (vertex_count == 0)
      return;

    std::unique_lock lock(self.mutex);
    auto& list = self.draw_lists[depth_tested];
    auto& vertices = primitive == Primitive::Lines ? list.line_vertices : list.triangle_vertices;
    const auto first = vertices.size();
    vertices.resize(first + vertex_count);
    fill(std::span<Vertex>(vertices).subspan(first));
  }

  auto get_view(this DebugRenderer& self) -> View;

  // moves everything queued since the last flush into vertices, lines first so a single buffer serves both
  // topologies, and remembers view for the next frame's text and lods
  auto flush(this DebugRenderer& self, const View& view, std::vector<Vertex>& vertices) -> DrawRanges;

private:
  struct Text {
    glm::vec3 position = {};
    f32 height = 0.0f;
    u32 color = 0;
    u32 offset = 0;
    u32 length = 0;
  };

  struct DrawList {
    std::vector<Vertex> line_vertices = {};
    std::vector<Vertex> triangle_vertices = {};
    std::vector<Text> texts = {};
    std::string text_chars = {};
  };

  std::shared_mutex mutex = {};
  // indexed by depth_tested
  std::array<DrawList, 2> draw_lists = {};
  View view = {};
};
} // namespace ox
