#pragma once

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyManager.h>
#include <Jolt/Renderer/DebugRenderer.h>
// clang-format on

#include "Render/DebugRenderer.hpp"

namespace JPH {
class PhysicsSystem;
}

namespace ox {
struct PhysicsDebugSettings {
  // hide what is behind scene geometry instead of drawing it dimmed
  bool depth_tested = false;
  JPH::BodyManager::DrawSettings bodies = {.mDrawShapeWireframe = true};
  bool constraints = false;
  bool constraint_limits = false;
  bool constraint_reference_frames = false;

  // jolt keeps the rest as globals and draws them from inside the physics step
  bool contact_points = false;
  bool contact_manifolds = false;
  bool contact_point_reduction = false;
  bool supporting_faces = false;
  bool motion_quality_linear_cast = false;
  bool submerged_volumes = false;
  bool mesh_triangle_groups = false;
  bool mesh_triangle_outlines = false;
  bool height_field_triangle_outlines = false;
  bool convex_hull_face_outlines = false;
};

// forwards everything jolt draws into the DebugRenderer of the scene being stepped or drawn. one instance lives in
// the Physics module since jolt's step-time drawing goes through the JPH::DebugRenderer::sInstance global
class PhysicsDebugRenderer final : public JPH::DebugRenderer {
public:
  PhysicsDebugSettings settings = {};
  // only set inside begin_step/end_step and draw
  ox::DebugRenderer* target = nullptr;
  // step output is retained so it doesn't flicker when the frame rate outpaces the fixed step
  bool recording_step = false;

  PhysicsDebugRenderer();

  // wrap PhysicsSystem::Update, enabled = false turns jolt's step-time drawing off
  auto begin_step(this PhysicsDebugRenderer& self, ox::DebugRenderer& debug_renderer, bool enabled) -> void;
  auto end_step(this PhysicsDebugRenderer& self) -> void;
  // draws bodies and constraints
  auto draw(this PhysicsDebugRenderer& self, JPH::PhysicsSystem& system, ox::DebugRenderer& debug_renderer) -> void;

  // virtual overrides can't take an explicit object parameter
  auto DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) -> void override;
  auto DrawTriangle(
    JPH::RVec3Arg inV1,
    JPH::RVec3Arg inV2,
    JPH::RVec3Arg inV3,
    JPH::ColorArg inColor,
    ECastShadow inCastShadow = ECastShadow::Off
  ) -> void override;
  auto CreateTriangleBatch(const Triangle* inTriangles, int inTriangleCount) -> Batch override;
  auto CreateTriangleBatch(const Vertex* inVertices, int inVertexCount, const u32* inIndices, int inIndexCount)
    -> Batch override;
  auto DrawGeometry(
    JPH::RMat44Arg inModelMatrix,
    const JPH::AABox& inWorldSpaceBounds,
    float inLODScaleSq,
    JPH::ColorArg inModelColor,
    const GeometryRef& inGeometry,
    ECullMode inCullMode,
    ECastShadow inCastShadow,
    EDrawMode inDrawMode
  ) -> void override;
  auto DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString, JPH::ColorArg inColor, float inHeight)
    -> void override;
};
} // namespace ox
