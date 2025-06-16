#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Renderer/DebugRenderer.h>

#include "Physics/RayCast.hpp"
#include "Render/BoundingVolume.hpp"
#include "Render/MeshVertex.hpp"

namespace ox {
class PhysicsDebugRenderer;

class DebugRenderer {
public:
  static constexpr uint32_t MAX_LINES = 10'000;
  static constexpr uint32_t MAX_LINE_VERTICES = MAX_LINES * 2;
  static constexpr uint32_t MAX_LINE_INDICES = MAX_LINES * 6;

  struct Line {
    glm::vec3 p1 = {};
    glm::vec3 p2 = {};
    glm::vec4 col = {};
  };

  struct Point {
    glm::vec3 p1 = {};
    glm::vec4 col = {};
    float size = 0;
  };

  struct Triangle {
    glm::vec3 p1 = {};
    glm::vec3 p2 = {};
    glm::vec3 p3 = {};
    glm::vec4 col = {};
  };

  DebugRenderer() = default;
  ~DebugRenderer() = default;

  static void init();
  static void release();
  static void reset(bool clear_depth_tested = true);

  /// Draw Point (circle)
  static void draw_point(const glm::vec3& pos,
                         float point_radius,
                         const glm::vec4& color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
                         bool depth_tested = false);

  /// Draw Line with a given thickness
  static void draw_line(const glm::vec3& start,
                        const glm::vec3& end,
                        float line_width,
                        const glm::vec4& color = glm::vec4(1),
                        bool depth_tested = false);
  static void draw_triangle(
      const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec4& color, bool depth_tested = false);

  static void draw_circle(int num_verts,
                          float radius,
                          const glm::vec3& position,
                          const glm::quat& rotation,
                          const glm::vec4& color,
                          bool depth_tested = false);
  static void draw_sphere(float radius, const glm::vec3& position, const glm::vec4& color, bool depth_tested = false);
  static void draw_capsule(const glm::vec3& position,
                           const glm::quat& rotation,
                           float height,
                           float radius,
                           const glm::vec4& color,
                           bool depth_tested = false);
  static void draw_cone(int num_circle_verts,
                        int num_lines_to_circle,
                        float angle,
                        float length,
                        const glm::vec3& position,
                        const glm::quat& rotation,
                        const glm::vec4& color,
                        bool depth_tested = false);
  static void draw_aabb(const AABB& aabb,
                        const glm::vec4& color = glm::vec4(1.0f),
                        bool corners_only = false,
                        float width = 1.0f,
                        bool depth_tested = false);
  static void draw_frustum(const glm::mat4& frustum, const glm::vec4& color, float near, float far);
  static void
  draw_ray(const RayCast& ray, const glm::vec4& color, const float distance, const bool depth_tested = false);

  static DebugRenderer* get_instance() { return instance; }
  const std::vector<Line>& get_lines(bool depth_tested = true) const {
    return !depth_tested ? draw_list.debug_lines : draw_list_depth_tested.debug_lines;
  }
  const std::vector<Triangle>& get_triangles(bool depth_tested = true) const {
    return !depth_tested ? draw_list.debug_triangles : draw_list_depth_tested.debug_triangles;
  }
  const std::vector<Point>& get_points(bool depth_tested = true) const {
    return !depth_tested ? draw_list.debug_points : draw_list_depth_tested.debug_points;
  }

  const vuk::Unique<vuk::Buffer>& get_global_index_buffer() const { return debug_renderer_context.index_buffer; }

  static std::pair<std::vector<Vertex>, uint32_t> get_vertices_from_lines(const std::vector<Line>& lines);
  static std::pair<std::vector<Vertex>, uint32_t> get_vertices_from_triangles(const std::vector<Triangle>& triangles);

private:
  static DebugRenderer* instance;

  friend PhysicsDebugRenderer;

  struct DebugDrawList {
    std::vector<Line> debug_lines = {};
    std::vector<Point> debug_points = {};
    std::vector<Triangle> debug_triangles = {};
  };

  struct DebugRendererContext {
    vuk::Unique<vuk::Buffer> index_buffer;
  } debug_renderer_context;

  DebugDrawList draw_list;
  DebugDrawList draw_list_depth_tested;
};

class PhysicsDebugRenderer final : public JPH::DebugRenderer {
public:
  bool draw_depth_tested = false; // TODO: configurable via cvar

  struct TriangleBatch : public JPH::RefTargetVirtual {
    std::vector<ox::DebugRenderer::Triangle> triangles;

    int ref_count = 0;

    virtual void AddRef() override { ++ref_count; }

    virtual void Release() override {
      --ref_count;

      if (ref_count == 0) {
        auto* pThis = this;
        delete pThis;
      }
    }
  };

  PhysicsDebugRenderer();

  virtual void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override;
  virtual void DrawTriangle(JPH::RVec3Arg inV1,
                            JPH::RVec3Arg inV2,
                            JPH::RVec3Arg inV3,
                            JPH::ColorArg inColor,
                            ECastShadow inCastShadow = ECastShadow::Off) override;
  virtual Batch CreateTriangleBatch(const Triangle* inTriangles, int inTriangleCount) override;
  virtual Batch
  CreateTriangleBatch(const Vertex* inVertices, int inVertexCount, const u32* inIndices, int inIndexCount) override;
  virtual void DrawGeometry(JPH::RMat44Arg inModelMatrix,
                            const JPH::AABox& inWorldSpaceBounds,
                            float inLODScaleSq,
                            JPH::ColorArg inModelColor,
                            const GeometryRef& inGeometry,
                            ECullMode inCullMode,
                            ECastShadow inCastShadow,
                            EDrawMode inDrawMode) override;
  virtual void DrawText3D(JPH::RVec3Arg inPosition,
                          const std::string_view& inString,
                          JPH::ColorArg inColor,
                          float inHeight) override;
};
} // namespace ox
