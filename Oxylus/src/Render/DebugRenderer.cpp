#include "DebugRenderer.hpp"

#include <vuk/vsl/Core.hpp>

#include "Core/App.hpp"
#include "Utils/OxMath.hpp"
#include "Vulkan/VkContext.hpp"

namespace ox {
DebugRenderer* DebugRenderer::instance = nullptr;

void DebugRenderer::init() {
  ZoneScoped;
  if (instance)
    return;

  instance = new DebugRenderer();

  std::vector<uint32_t> indices = {};
  indices.resize(MAX_LINE_INDICES);

  for (uint32_t i = 0; i < MAX_LINE_INDICES; i++) {
    indices[i] = i;
  }

  auto [i_buff, i_buff_fut] = create_buffer(*App::get_vkcontext().superframe_allocator,
                                            vuk::MemoryUsage::eCPUtoGPU,
                                            vuk::DomainFlagBits::eTransferOnGraphics,
                                            std::span(indices));

  auto compiler = vuk::Compiler{};
  i_buff_fut.wait(*App::get_vkcontext().superframe_allocator, compiler);

  instance->debug_renderer_context.index_buffer = std::move(i_buff);
}

void DebugRenderer::release() {
  delete instance;
  instance = nullptr;
}

void DebugRenderer::reset(bool clear_depth_tested) {
  ZoneScoped;
  instance->draw_list.debug_lines.clear();
  instance->draw_list.debug_points.clear();

  if (clear_depth_tested) {
    instance->draw_list_depth_tested.debug_lines.clear();
    instance->draw_list_depth_tested.debug_points.clear();
  }
}

void DebugRenderer::draw_point(const glm::vec3& pos, float point_radius, const glm::vec4& color, bool depth_tested) {
  ZoneScoped;
  if (depth_tested)
    instance->draw_list_depth_tested.debug_points.emplace_back(Point{pos, color, point_radius});
  else
    instance->draw_list.debug_points.emplace_back(Point{pos, color, point_radius});
}

void DebugRenderer::draw_line(
    const glm::vec3& start, const glm::vec3& end, float line_width, const glm::vec4& color, bool depth_tested) {
  ZoneScoped;
  if (depth_tested)
    instance->draw_list_depth_tested.debug_lines.emplace_back(Line{start, end, color});
  else
    instance->draw_list.debug_lines.emplace_back(Line{start, end, color});
}

void DebugRenderer::draw_triangle(
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec4& color, bool depth_tested) {
  ZoneScoped;
  if (depth_tested)
    instance->draw_list_depth_tested.debug_triangles.emplace_back(Triangle{v0, v1, v2, color});
  else
    instance->draw_list.debug_triangles.emplace_back(Triangle{v0, v1, v2, color});
}

void DebugRenderer::draw_circle(int num_verts,
                                float radius,
                                const glm::vec3& position,
                                const glm::quat& rotation,
                                const glm::vec4& color,
                                bool depth_tested) {
  float step = 360.0f / float(num_verts);

  for (int i = 0; i < num_verts; i++) {
    float cx = glm::cos(step * i) * radius;
    float cy = glm::sin(step * i) * radius;
    glm::vec3 current = glm::vec3(cx, cy, 0.0f);

    float nx = glm::cos(step * (i + 1)) * radius;
    float ny = glm::sin(step * (i + 1)) * radius;
    glm::vec3 next = glm::vec3(nx, ny, 0.0f);

    draw_line(position + (rotation * current), position + (rotation * next), 1.0f, color, depth_tested);
  }
}

void DebugRenderer::draw_sphere(float radius, const glm::vec3& position, const glm::vec4& color, bool depth_tested) {
  draw_circle(20, radius, position, glm::quat(glm::vec3(0.0f, 0.0f, 0.0f)), color, depth_tested);
  draw_circle(20, radius, position, glm::quat(glm::vec3(90.0f, 0.0f, 0.0f)), color, depth_tested);
  draw_circle(20, radius, position, glm::quat(glm::vec3(0.0f, 90.0f, 90.0f)), color, depth_tested);
}

void draw_arc(int num_verts,
              float radius,
              const glm::vec3& start,
              const glm::vec3& end,
              const glm::quat& rotation,
              const glm::vec4& color,
              bool depth_tested) {
  float step = 180.0f / num_verts;
  glm::quat rot = glm::lookAt(rotation * start, rotation * end, glm::vec3(0.0f, 1.0f, 0.0f));
  rot = rotation * rot;

  glm::vec3 arcCentre = (start + end) * 0.5f;
  for (int i = 0; i < num_verts; i++) {
    float cx = glm::cos(step * i) * radius;
    float cy = glm::sin(step * i) * radius;
    glm::vec3 current = glm::vec3(cx, cy, 0.0f);

    float nx = glm::cos(step * (i + 1)) * radius;
    float ny = glm::sin(step * (i + 1)) * radius;
    glm::vec3 next = glm::vec3(nx, ny, 0.0f);

    DebugRenderer::draw_line(arcCentre + (rot * current), arcCentre + (rot * next), 1.0f, color, depth_tested);
  }
}

void DebugRenderer::draw_capsule(const glm::vec3& position,
                                 const glm::quat& rotation,
                                 float height,
                                 float radius,
                                 const glm::vec4& color,
                                 bool depth_tested) {
  glm::vec3 up = (rotation * glm::vec3(0.0f, 1.0f, 0.0f));

  glm::vec3 top_sphere_Centre = position + up * (height * 0.5f);
  glm::vec3 bottom_sphere_centre = position - up * (height * 0.5f);

  draw_circle(20,
              radius,
              top_sphere_Centre,
              rotation * glm::quat(glm::vec3(glm::radians(90.0f), 0.0f, 0.0f)),
              color,
              depth_tested);
  draw_circle(20,
              radius,
              bottom_sphere_centre,
              rotation * glm::quat(glm::vec3(glm::radians(90.0f), 0.0f, 0.0f)),
              color,
              depth_tested);

  // Draw 10 arcs
  // Sides
  float step = 360.0f / float(20);
  for (int i = 0; i < 20; i++) {
    float z = glm::cos(step * i) * radius;
    float x = glm::sin(step * i) * radius;

    glm::vec3 offset = rotation * glm::vec4(x, 0.0f, z, 0.0f);
    draw_line(bottom_sphere_centre + offset, top_sphere_Centre + offset, 1.0f, color, depth_tested);

    if (i < 10) {
      float z2 = glm::cos(step * (i + 10)) * radius;
      float x2 = glm::sin(step * (i + 10)) * radius;

      glm::vec3 offset2 = rotation * glm::vec4(x2, 0.0f, z2, 0.0f);
      // Top Hemishpere
      draw_arc(20, radius, top_sphere_Centre + offset, top_sphere_Centre + offset2, rotation, color, depth_tested);
      // Bottom Hemisphere
      draw_arc(20,
               radius,
               bottom_sphere_centre + offset,
               bottom_sphere_centre + offset2,
               rotation * glm::quat(glm::vec3(glm::radians(180.0f), 0.0f, 0.0f)),
               color,
               depth_tested);
    }
  }
}

void DebugRenderer::draw_cone(int num_circle_verts,
                              int num_lines_to_circle,
                              float angle,
                              float length,
                              const glm::vec3& position,
                              const glm::quat& rotation,
                              const glm::vec4& color,
                              bool depth_tested) {
  float endAngle = glm::tan(angle * 0.5f) * length;
  glm::vec3 forward = -(rotation * glm::vec3(0.0f, 0.0f, -1.0f));
  glm::vec3 endPosition = position + forward * length;
  draw_circle(num_circle_verts, endAngle, endPosition, rotation, color, depth_tested);

  for (int i = 0; i < num_lines_to_circle; i++) {
    float a = i * 90.0f;
    glm::vec3 point = rotation * glm::vec3(glm::cos(a), glm::sin(a), 0.0f) * endAngle;
    draw_line(position, position + point + forward * length, false, color);
  }
}

void
DebugRenderer::draw_aabb(const AABB& aabb, const glm::vec4& color, bool corners_only, float width, bool depth_tested) {
  glm::vec3 uuu = aabb.max;
  glm::vec3 lll = aabb.min;

  glm::vec3 ull(uuu.x, lll.y, lll.z);
  glm::vec3 uul(uuu.x, uuu.y, lll.z);
  glm::vec3 ulu(uuu.x, lll.y, uuu.z);

  glm::vec3 luu(lll.x, uuu.y, uuu.z);
  glm::vec3 llu(lll.x, lll.y, uuu.z);
  glm::vec3 lul(lll.x, uuu.y, lll.z);

  // Draw edges
  if (!corners_only) {
    draw_line(luu, uuu, width, color, depth_tested);
    draw_line(lul, uul, width, color, depth_tested);
    draw_line(llu, ulu, width, color, depth_tested);
    draw_line(lll, ull, width, color, depth_tested);

    draw_line(lul, lll, width, color, depth_tested);
    draw_line(uul, ull, width, color, depth_tested);
    draw_line(luu, llu, width, color, depth_tested);
    draw_line(uuu, ulu, width, color, depth_tested);

    draw_line(lll, llu, width, color, depth_tested);
    draw_line(ull, ulu, width, color, depth_tested);
    draw_line(lul, luu, width, color, depth_tested);
    draw_line(uul, uuu, width, color, depth_tested);
  } else {
    draw_line(luu, luu + (uuu - luu) * 0.25f, width, color, depth_tested);
    draw_line(luu + (uuu - luu) * 0.75f, uuu, width, color, depth_tested);

    draw_line(lul, lul + (uul - lul) * 0.25f, width, color, depth_tested);
    draw_line(lul + (uul - lul) * 0.75f, uul, width, color, depth_tested);

    draw_line(llu, llu + (ulu - llu) * 0.25f, width, color, depth_tested);
    draw_line(llu + (ulu - llu) * 0.75f, ulu, width, color, depth_tested);

    draw_line(lll, lll + (ull - lll) * 0.25f, width, color, depth_tested);
    draw_line(lll + (ull - lll) * 0.75f, ull, width, color, depth_tested);

    draw_line(lul, lul + (lll - lul) * 0.25f, width, color, depth_tested);
    draw_line(lul + (lll - lul) * 0.75f, lll, width, color, depth_tested);

    draw_line(uul, uul + (ull - uul) * 0.25f, width, color, depth_tested);
    draw_line(uul + (ull - uul) * 0.75f, ull, width, color, depth_tested);

    draw_line(luu, luu + (llu - luu) * 0.25f, width, color, depth_tested);
    draw_line(luu + (llu - luu) * 0.75f, llu, width, color, depth_tested);

    draw_line(uuu, uuu + (ulu - uuu) * 0.25f, width, color, depth_tested);
    draw_line(uuu + (ulu - uuu) * 0.75f, ulu, width, color, depth_tested);

    draw_line(lll, lll + (llu - lll) * 0.25f, width, color, depth_tested);
    draw_line(lll + (llu - lll) * 0.75f, llu, width, color, depth_tested);

    draw_line(ull, ull + (ulu - ull) * 0.25f, width, color, depth_tested);
    draw_line(ull + (ulu - ull) * 0.75f, ulu, width, color, depth_tested);

    draw_line(lul, lul + (luu - lul) * 0.25f, width, color, depth_tested);
    draw_line(lul + (luu - lul) * 0.75f, luu, width, color, depth_tested);

    draw_line(uul, uul + (uuu - uul) * 0.25f, width, color, depth_tested);
    draw_line(uul + (uuu - uul) * 0.75f, uuu, width, color, depth_tested);
  }
}

void DebugRenderer::draw_frustum(const glm::mat4& frustum, const glm::vec4& color, float near, float far) {
  // Get frustum corners in world space
  auto tln = math::unproject_uv_zo(near, {0, 1}, frustum);
  auto trn = math::unproject_uv_zo(near, {1, 1}, frustum);
  auto bln = math::unproject_uv_zo(near, {0, 0}, frustum);
  auto brn = math::unproject_uv_zo(near, {1, 0}, frustum);

  // Far corners are lerped slightly to near in case it is an infinite projection
  auto tlf = math::unproject_uv_zo(glm::mix(far, near, 1e-5), {0, 1}, frustum);
  auto trf = math::unproject_uv_zo(glm::mix(far, near, 1e-5), {1, 1}, frustum);
  auto blf = math::unproject_uv_zo(glm::mix(far, near, 1e-5), {0, 0}, frustum);
  auto brf = math::unproject_uv_zo(glm::mix(far, near, 1e-5), {1, 0}, frustum);

  // Connect-the-dots
  // Near and far "squares"
  draw_line(tln, trn, 1.0f, color, true);
  draw_line(bln, brn, 1.0f, color, true);
  draw_line(tln, bln, 1.0f, color, true);
  draw_line(trn, brn, 1.0f, color, true);
  draw_line(tlf, trf, 1.0f, color, true);
  draw_line(blf, brf, 1.0f, color, true);
  draw_line(tlf, blf, 1.0f, color, true);
  draw_line(trf, brf, 1.0f, color, true);

  // Lines connecting near and far planes
  draw_line(tln, tlf, 1.0f, color, true);
  draw_line(trn, trf, 1.0f, color, true);
  draw_line(bln, blf, 1.0f, color, true);
  draw_line(brn, brf, 1.0f, color, true);
}

void
DebugRenderer::draw_ray(const RayCast& ray, const glm::vec4& color, const float distance, const bool depth_tested) {
  draw_line(ray.get_origin(), ray.get_origin() + ray.get_direction() * distance, 1.0f, color, depth_tested);
}

std::pair<std::vector<Vertex>, uint32_t> DebugRenderer::get_vertices_from_lines(const std::vector<Line>& lines) {
  std::vector<Vertex> vertices = {};
  vertices.reserve(lines.size() * 2);
  uint32_t indices = 0;

  for (const auto& line : lines) {
    // store color in normals for simplicity
    vertices.emplace_back(
        Vertex{.position = line.p1, .normal = glm::packSnorm2x16(math::float32x3_to_oct(line.col)), .uv = {}});
    vertices.emplace_back(
        Vertex{.position = line.p2, .normal = glm::packSnorm2x16(math::float32x3_to_oct(line.col)), .uv = {}});

    indices += 2;
  }

  return {vertices, indices};
}

std::pair<std::vector<Vertex>, uint32_t>
DebugRenderer::get_vertices_from_triangles(const std::vector<Triangle>& triangles) {
  std::vector<Vertex> vertices = {};
  vertices.reserve(triangles.size() * 3);
  uint32_t indices = 0;

  for (const auto& tri : triangles) {
    // store color in normals for simplicity
    vertices.emplace_back(
        Vertex{.position = tri.p1, .normal = glm::packSnorm2x16(math::float32x3_to_oct(tri.col)), .uv = {}});
    vertices.emplace_back(
        Vertex{.position = tri.p2, .normal = glm::packSnorm2x16(math::float32x3_to_oct(tri.col)), .uv = {}});
    vertices.emplace_back(
        Vertex{.position = tri.p3, .normal = glm::packSnorm2x16(math::float32x3_to_oct(tri.col)), .uv = {}});

    indices += 3;
  }

  return {vertices, indices};
}

// ----------------------
// Physics Debug Renderer

PhysicsDebugRenderer::PhysicsDebugRenderer() { DebugRenderer::Initialize(); }

void PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) {
  ox::DebugRenderer::draw_line(
      math::from_jolt(inFrom), math::from_jolt(inTo), 1.0f, math::from_jolt(inColor.ToVec4()), draw_depth_tested);
}

void PhysicsDebugRenderer::DrawTriangle(
    JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3, JPH::ColorArg inColor, ECastShadow inCastShadow) {
  ox::DebugRenderer::draw_triangle(math::from_jolt(inV1),
                                   math::from_jolt(inV2),
                                   math::from_jolt(inV3),
                                   math::from_jolt(inColor.ToVec4()),
                                   draw_depth_tested);
}

JPH::DebugRenderer::Batch PhysicsDebugRenderer::CreateTriangleBatch(const Triangle* inTriangles, int inTriangleCount) {
  TriangleBatch* pBatch = new TriangleBatch;
  pBatch->triangles.reserve(inTriangleCount);

  for (int i = 0; i < inTriangleCount; ++i) {
    auto& t = pBatch->triangles.emplace_back();
    t.p1 = math::from_jolt(JPH::Vec3{inTriangles[i].mV[0].mPosition});
    t.p2 = math::from_jolt(JPH::Vec3{inTriangles[i].mV[1].mPosition});
    t.p3 = math::from_jolt(JPH::Vec3{inTriangles[i].mV[2].mPosition});
    t.col = math::from_jolt(inTriangles[i].mV[0].mColor.ToVec4());
  }

  return pBatch;
}

JPH::DebugRenderer::Batch PhysicsDebugRenderer::CreateTriangleBatch(const Vertex* inVertices,
                                                                    int inVertexCount,
                                                                    const u32* inIndices,
                                                                    int inIndexCount) {
  const u32 numTris = inIndexCount / 3;

  TriangleBatch* pBatch = new TriangleBatch;
  pBatch->triangles.reserve(numTris);

  u32 index = 0;

  for (u32 i = 0; i < numTris; ++i) {
    auto& t = pBatch->triangles.emplace_back();
    t.p1 = math::from_jolt(JPH::Vec3{inVertices[inIndices[index + 0]].mPosition});
    t.p2 = math::from_jolt(JPH::Vec3{inVertices[inIndices[index + 1]].mPosition});
    t.p3 = math::from_jolt(JPH::Vec3{inVertices[inIndices[index + 2]].mPosition});
    t.col = math::from_jolt(inVertices[inIndices[index + 0]].mColor.ToVec4());

    index += 3;
  }

  return pBatch;
}

void PhysicsDebugRenderer::DrawGeometry(JPH::RMat44Arg inModelMatrix,
                                        const JPH::AABox& inWorldSpaceBounds,
                                        float inLODScaleSq,
                                        JPH::ColorArg inModelColor,
                                        const GeometryRef& geometry,
                                        ECullMode inCullMode,
                                        ECastShadow inCastShadow,
                                        EDrawMode inDrawMode) {
  if (geometry == nullptr)
    return;

  u32 uiLod = 0;
  if (geometry->mLODs.size() > 1)
    uiLod = 1;
  if (geometry->mLODs.size() > 2)
    uiLod = 2;

  const TriangleBatch* pBatch = static_cast<const TriangleBatch*>(geometry->mLODs[uiLod].mTriangleBatch.GetPtr());

  const glm::mat4 trans = reinterpret_cast<const glm::mat4&>(inModelMatrix);
  const glm::vec4 color = math::from_jolt(inModelColor.ToVec4());

  // TODO: currently only renders into not depth tested list...
  auto debug_renderer = ox::DebugRenderer::get_instance();

  if (inDrawMode == JPH::DebugRenderer::EDrawMode::Solid) {
    if (inCullMode == JPH::DebugRenderer::ECullMode::CullBackFace || inCullMode == JPH::DebugRenderer::ECullMode::Off) {
      for (u32 t = 0; t < pBatch->triangles.size(); ++t) {
        auto& tri = debug_renderer->draw_list.debug_triangles.emplace_back();
        tri.col = pBatch->triangles[t].col * color;
        tri.p1 = trans * glm::vec4(pBatch->triangles[t].p1, 1.f);
        tri.p2 = trans * glm::vec4(pBatch->triangles[t].p2, 1.f);
        tri.p3 = trans * glm::vec4(pBatch->triangles[t].p3, 1.f);
      }
    }

    if (inCullMode == JPH::DebugRenderer::ECullMode::CullFrontFace ||
        inCullMode == JPH::DebugRenderer::ECullMode::Off) {
      for (u32 t = 0; t < pBatch->triangles.size(); ++t) {
        auto& tri = debug_renderer->draw_list.debug_triangles.emplace_back();
        tri.col = pBatch->triangles[t].col * color;
        tri.p1 = trans * glm::vec4(pBatch->triangles[t].p1, 1.f);
        tri.p2 = trans * glm::vec4(pBatch->triangles[t].p3, 1.f);
        tri.p3 = trans * glm::vec4(pBatch->triangles[t].p2, 1.f);
      }
    }
  } else {
    for (u32 t = 0; t < pBatch->triangles.size(); ++t) {
      const auto& tri = pBatch->triangles[t];
      const auto col = pBatch->triangles[t].col * color;

      const glm::vec3 v0 = trans * glm::vec4(tri.p1, 1.0f);
      const glm::vec3 v1 = trans * glm::vec4(tri.p2, 1.0f);
      const glm::vec3 v2 = trans * glm::vec4(tri.p3, 1.0f);

      debug_renderer->draw_list.debug_lines.emplace_back(ox::DebugRenderer::Line{v0, v1, col});
      debug_renderer->draw_list.debug_lines.emplace_back(ox::DebugRenderer::Line{v1, v2, col});
      debug_renderer->draw_list.debug_lines.emplace_back(ox::DebugRenderer::Line{v2, v0, col});
    }
  }
}

void PhysicsDebugRenderer::DrawText3D(JPH::RVec3Arg inPosition,
                                      const std::string_view& inString,
                                      JPH::ColorArg inColor,
                                      float inHeight) {}
} // namespace ox
