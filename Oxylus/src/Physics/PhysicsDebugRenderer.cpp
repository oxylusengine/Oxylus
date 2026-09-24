#include "Physics/PhysicsDebugRenderer.hpp"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <algorithm>
#include <atomic>

#include "Core/App.hpp"
#include "Utils/OxMath.hpp"

namespace ox {
// model space, unindexed, three vertices per triangle
struct PhysicsTriangleBatch final : JPH::RefTargetVirtual {
  std::vector<DebugRenderer::Vertex> vertices = {};
  std::atomic<u32> ref_count = 0;

  auto AddRef() -> void override { ref_count.fetch_add(1, std::memory_order_relaxed); }

  auto Release() -> void override {
    // acq_rel so the deleting thread sees every other holder's writes
    if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete this;
  }
};

template <typename F>
static auto emit_primitives(
  PhysicsDebugRenderer& self, const DebugRenderer::Primitive primitive, const usize vertex_count, F&& fill
) -> void {
  if (self.recording_step) {
    std::unique_lock lock(self.step_mutex);
    auto& vertices = primitive == DebugRenderer::Primitive::Lines ? self.step_lines : self.step_triangles;
    const auto first = vertices.size();
    vertices.resize(first + vertex_count);
    fill(std::span<DebugRenderer::Vertex>(vertices).subspan(first));
    return;
  }

  App::mod<DebugRenderer>().emit(primitive, vertex_count, self.settings.depth_tested, std::forward<F>(fill));
}

static auto to_glm(JPH::RMat44Arg matrix) -> glm::mat4 {
  return glm::mat4(
    math::from_jolt(matrix.GetColumn4(0)),
    math::from_jolt(matrix.GetColumn4(1)),
    math::from_jolt(matrix.GetColumn4(2)),
    math::from_jolt(matrix.GetColumn4(3))
  );
}

PhysicsDebugRenderer::PhysicsDebugRenderer() { Initialize(); }

auto PhysicsDebugRenderer::begin_step(this PhysicsDebugRenderer& self, const bool enabled) -> void {
  const auto& settings = self.settings;
  JPH::ContactConstraintManager::sDrawContactPoint = enabled && settings.contact_points;
  JPH::ContactConstraintManager::sDrawSupportingFaces = enabled && settings.supporting_faces;
  JPH::ContactConstraintManager::sDrawContactPointReduction = enabled && settings.contact_point_reduction;
  JPH::ContactConstraintManager::sDrawContactManifolds = enabled && settings.contact_manifolds;
  JPH::PhysicsSystem::sDrawMotionQualityLinearCast = enabled && settings.motion_quality_linear_cast;
  JPH::Shape::sDrawSubmergedVolumes = enabled && settings.submerged_volumes;

  // jolt's destructor nulls this, so don't rely on the constructor having set it
  JPH::DebugRenderer::sInstance = &self;

  std::unique_lock lock(self.step_mutex);
  self.step_lines.clear();
  self.step_triangles.clear();
  self.step_texts.clear();
  self.recording_step = enabled;
}

auto PhysicsDebugRenderer::end_step(this PhysicsDebugRenderer& self) -> void { self.recording_step = false; }

auto PhysicsDebugRenderer::draw(this PhysicsDebugRenderer& self, JPH::PhysicsSystem& system, const bool replay_step)
  -> void {
  ZoneScoped;

  const auto& settings = self.settings;
  JPH::MeshShape::sDrawTriangleGroups = settings.mesh_triangle_groups;
  JPH::MeshShape::sDrawTriangleOutlines = settings.mesh_triangle_outlines;
  JPH::HeightFieldShape::sDrawTriangleOutlines = settings.height_field_triangle_outlines;
  JPH::ConvexHullShape::sDrawFaceOutlines = settings.convex_hull_face_outlines;

  system.DrawBodies(settings.bodies, &self);
  if (settings.constraints)
    system.DrawConstraints(&self);
  if (settings.constraint_limits)
    system.DrawConstraintLimits(&self);
  if (settings.constraint_reference_frames)
    system.DrawConstraintReferenceFrame(&self);

  if (!replay_step)
    return;

  auto& debug_renderer = App::mod<ox::DebugRenderer>();
  std::unique_lock lock(self.step_mutex);
  debug_renderer.emit(
    ox::DebugRenderer::Primitive::Lines,
    self.step_lines.size(),
    settings.depth_tested,
    [&](std::span<ox::DebugRenderer::Vertex> out) { std::ranges::copy(self.step_lines, out.begin()); }
  );
  debug_renderer.emit(
    ox::DebugRenderer::Primitive::Triangles,
    self.step_triangles.size(),
    settings.depth_tested,
    [&](std::span<ox::DebugRenderer::Vertex> out) { std::ranges::copy(self.step_triangles, out.begin()); }
  );
  for (const auto& text : self.step_texts) {
    debug_renderer.draw_text(
      math::from_jolt(JPH::Vec3(text.position)),
      text.text,
      text.height,
      math::from_jolt(text.color.ToVec4()),
      settings.depth_tested
    );
  }
}

auto PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) -> void {
  emit_primitives(*this, ox::DebugRenderer::Primitive::Lines, 2, [&](std::span<ox::DebugRenderer::Vertex> out) {
    out[0] = {math::from_jolt(JPH::Vec3(inFrom)), inColor.GetUInt32()};
    out[1] = {math::from_jolt(JPH::Vec3(inTo)), inColor.GetUInt32()};
  });
}

auto PhysicsDebugRenderer::DrawTriangle(
  JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3, JPH::ColorArg inColor, ECastShadow
) -> void {
  emit_primitives(*this, ox::DebugRenderer::Primitive::Triangles, 3, [&](std::span<ox::DebugRenderer::Vertex> out) {
    out[0] = {math::from_jolt(JPH::Vec3(inV1)), inColor.GetUInt32()};
    out[1] = {math::from_jolt(JPH::Vec3(inV2)), inColor.GetUInt32()};
    out[2] = {math::from_jolt(JPH::Vec3(inV3)), inColor.GetUInt32()};
  });
}

auto PhysicsDebugRenderer::CreateTriangleBatch(const Triangle* inTriangles, int inTriangleCount) -> Batch {
  auto* batch = new PhysicsTriangleBatch;
  batch->vertices.reserve(static_cast<usize>(inTriangleCount) * 3);

  for (i32 i = 0; i < inTriangleCount; i++) {
    for (const auto& vertex : inTriangles[i].mV) {
      batch->vertices.push_back({math::from_jolt(JPH::Vec3(vertex.mPosition)), vertex.mColor.GetUInt32()});
    }
  }

  return batch;
}

auto PhysicsDebugRenderer::CreateTriangleBatch(const Vertex* inVertices, int, const u32* inIndices, int inIndexCount)
  -> Batch {
  auto* batch = new PhysicsTriangleBatch;
  batch->vertices.reserve(static_cast<usize>(inIndexCount));

  for (i32 i = 0; i < inIndexCount; i++) {
    const auto& vertex = inVertices[inIndices[i]];
    batch->vertices.push_back({math::from_jolt(JPH::Vec3(vertex.mPosition)), vertex.mColor.GetUInt32()});
  }

  return batch;
}

auto PhysicsDebugRenderer::DrawGeometry(
  JPH::RMat44Arg inModelMatrix,
  const JPH::AABox& inWorldSpaceBounds,
  float inLODScaleSq,
  JPH::ColorArg inModelColor,
  const GeometryRef& inGeometry,
  ECullMode inCullMode,
  ECastShadow,
  EDrawMode inDrawMode
) -> void {
  ZoneScoped;

  if (inGeometry == nullptr || inGeometry->mLODs.empty())
    return;

  const auto camera_position = math::to_jolt(App::mod<ox::DebugRenderer>().get_view().position);
  const auto& lod = inGeometry->GetLOD(camera_position, inWorldSpaceBounds, inLODScaleSq);
  const auto* batch = static_cast<const PhysicsTriangleBatch*>(lod.mTriangleBatch.GetPtr());
  if (batch == nullptr || batch->vertices.empty())
    return;

  const auto transform = to_glm(inModelMatrix);
  const auto vertex = [&](const usize i) -> ox::DebugRenderer::Vertex {
    const auto& v = batch->vertices[i];
    return {glm::vec3(transform * glm::vec4(v.position, 1.0f)), (JPH::Color(v.color) * inModelColor).GetUInt32()};
  };
  const auto triangle_count = batch->vertices.size() / 3;

  if (inDrawMode == EDrawMode::Wireframe) {
    emit_primitives(
      *this,
      ox::DebugRenderer::Primitive::Lines,
      triangle_count * 6,
      [&](std::span<ox::DebugRenderer::Vertex> out) {
        for (usize t = 0; t < triangle_count; t++) {
          const auto a = vertex(t * 3 + 0);
          const auto b = vertex(t * 3 + 1);
          const auto c = vertex(t * 3 + 2);
          const auto edges = out.subspan(t * 6, 6);
          edges[0] = a;
          edges[1] = b;
          edges[2] = b;
          edges[3] = c;
          edges[4] = c;
          edges[5] = a;
        }
      }
    );
    return;
  }

  // the pipeline culls back faces, so culling front faces means drawing flipped triangles
  const auto draw_front = inCullMode != ECullMode::CullFrontFace;
  const auto draw_back = inCullMode != ECullMode::CullBackFace;
  const auto sides = static_cast<usize>(draw_front) + static_cast<usize>(draw_back);

  emit_primitives(
    *this,
    ox::DebugRenderer::Primitive::Triangles,
    triangle_count * 3 * sides,
    [&](std::span<ox::DebugRenderer::Vertex> out) {
      usize k = 0;
      for (usize t = 0; t < triangle_count; t++) {
        const auto a = vertex(t * 3 + 0);
        const auto b = vertex(t * 3 + 1);
        const auto c = vertex(t * 3 + 2);
        if (draw_front) {
          out[k++] = a;
          out[k++] = b;
          out[k++] = c;
        }
        if (draw_back) {
          out[k++] = a;
          out[k++] = c;
          out[k++] = b;
        }
      }
    }
  );
}

auto PhysicsDebugRenderer::DrawText3D(
  JPH::RVec3Arg inPosition, const std::string_view& inString, JPH::ColorArg inColor, float inHeight
) -> void {
  if (recording_step) {
    std::unique_lock lock(step_mutex);
    step_texts.push_back({.position = inPosition, .text = std::string(inString), .color = inColor, .height = inHeight});
    return;
  }

  App::mod<ox::DebugRenderer>().draw_text(
    math::from_jolt(JPH::Vec3(inPosition)),
    inString,
    inHeight,
    math::from_jolt(inColor.ToVec4()),
    settings.depth_tested
  );
}
} // namespace ox
