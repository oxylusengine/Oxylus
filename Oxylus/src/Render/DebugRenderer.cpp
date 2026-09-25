#include "Render/DebugRenderer.hpp"

#include <glm/ext/scalar_constants.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ox {
// stroke font on a 5x7 grid: each glyph is polylines of (x, y) digit pairs separated by spaces, y up from the
// baseline. covers ascii ' ' to '_', lowercase is drawn as uppercase
static constexpr std::array<std::string_view, 64> GLYPHS = {
  "",                              // ' '
  "2622 2021",                     // !
  "1615 3635",                     // "
  "1016 3036 0444 0242",           // #
  "453616050413334241301001 2026", // $
  "0046 0506 4041",                // %
  "40141526353401102042",          // &
  "2625",                          // '
  "36252130",                      // (
  "16252110",                      // )
  "1234 1432 2224",                // *
  "0343 2125",                     // +
  "2110",                          // ,
  "1333",                          // -
  "2021",                          // .
  "0046",                          // /
  "0006464000 0046",               // 0
  "152620 1030",                   // 1
  "064643030040",                  // 2
  "06464000 1343",                 // 3
  "060343 4640",                   // 4
  "460603434000",                  // 5
  "460600404303",                  // 6
  "064610",                        // 7
  "0006464000 0343",               // 8
  "004046060343",                  // 9
  "2122 2425",                     // :
  "2425 2210",                     // ;
  "351331",                        // <
  "0242 0444",                     // =
  "153311",                        // >
  "0646442322 2021",               // ?
  "32222434314146060040",          // @
  "0004264440 0343",               // A
  "00063645443303 3342413000",     // B
  "46060040",                      // C
  "00062644422000",                // D
  "46060040 0333",                 // E
  "460600 0333",                   // F
  "460600404323",                  // G
  "0006 4640 0343",                // H
  "0646 2620 0040",                // I
  "46400002",                      // J
  "0006 460340",                   // K
  "060040",                        // L
  "0006244640",                    // M
  "00064046",                      // N
  "0006464000",                    // O
  "0006464303",                    // P
  "0006464000 2240",               // Q
  "0006464303 2340",               // R
  "453616050413334241301001",      // S
  "0646 2620",                     // T
  "06004046",                      // U
  "062046",                        // V
  "0600224046",                    // W
  "0046 0640",                     // X
  "062346 2320",                   // Y
  "06460040",                      // Z
  "36161030",                      // [
  "0640",                          // backslash
  "16363010",                      // ]
  "142634",                        // ^
  "0040",                          // _
};

static constexpr f32 GLYPH_HEIGHT = 6.0f;
static constexpr f32 GLYPH_ADVANCE = 6.0f;
static constexpr f32 GLYPH_GAP = GLYPH_ADVANCE - 4.0f;
static constexpr f32 LINE_ADVANCE = 9.0f;

static auto find_glyph(c8 c) -> std::string_view {
  if (c >= 'a' && c <= 'z')
    c = static_cast<c8>(c - 'a' + 'A');

  switch (c) {
    case '|': return "2026";
    case '{': return GLYPHS['(' - ' '];
    case '}': return GLYPHS[')' - ' '];
    case '~': return "0314233243";
    case '`': return "1625";
    default : break;
  }

  if (c < ' ' || c > '_')
    return GLYPHS['?' - ' '];
  return GLYPHS[static_cast<usize>(c - ' ')];
}

static auto append_text(
  std::vector<DebugRenderer::Vertex>& vertices,
  const DebugRenderer::View& view,
  const glm::vec3& position,
  const f32 height,
  const u32 color,
  std::string_view text
) -> void {
  u32 line_count = 1;
  u32 columns = 0;
  u32 widest = 0;
  for (const auto c : text) {
    if (c == '\n') {
      line_count += 1;
      columns = 0;
    } else {
      widest = glm::max(widest, ++columns);
    }
  }

  const auto unit = height / GLYPH_HEIGHT;
  const auto right = view.right * unit;
  const auto up = view.up * unit;
  const auto block_width = static_cast<f32>(widest) * GLYPH_ADVANCE - GLYPH_GAP;
  const auto block_height = static_cast<f32>(line_count) * LINE_ADVANCE - (LINE_ADVANCE - GLYPH_HEIGHT);

  auto cursor_x = -block_width * 0.5f;
  auto cursor_y = block_height * 0.5f - GLYPH_HEIGHT;
  const auto grid_point = [&](const c8 x, const c8 y) {
    return position + right * (cursor_x + static_cast<f32>(x - '0')) + up * (cursor_y + static_cast<f32>(y - '0'));
  };

  for (const auto c : text) {
    if (c == '\n') {
      cursor_x = -block_width * 0.5f;
      cursor_y -= LINE_ADVANCE;
      continue;
    }

    const auto glyph = find_glyph(c);
    for (usize i = 0; i + 1 < glyph.size();) {
      if (glyph[i] == ' ') {
        i += 1;
        continue;
      }

      // a pair followed by another pair in the same stroke is a segment
      if (i + 3 < glyph.size() && glyph[i + 2] != ' ') {
        vertices.push_back({grid_point(glyph[i], glyph[i + 1]), color});
        vertices.push_back({grid_point(glyph[i + 2], glyph[i + 3]), color});
      }
      i += 2;
    }

    cursor_x += GLYPH_ADVANCE;
  }
}

static auto draw_arc(
  DebugRenderer& self,
  const glm::vec3& center,
  const glm::vec3& axis_x,
  const glm::vec3& axis_y,
  const f32 radius,
  const f32 start_angle,
  const f32 end_angle,
  const i32 segments,
  const glm::vec4& color,
  const bool depth_tested
) -> void {
  if (segments <= 0)
    return;

  const auto packed = DebugRenderer::pack_color(color);
  const auto step = (end_angle - start_angle) / static_cast<f32>(segments);
  const auto point = [&](const i32 i) {
    const auto angle = start_angle + step * static_cast<f32>(i);
    return center + (axis_x * glm::cos(angle) + axis_y * glm::sin(angle)) * radius;
  };

  self.emit(
    DebugRenderer::Primitive::Lines,
    static_cast<usize>(segments) * 2,
    depth_tested,
    [&](std::span<DebugRenderer::Vertex> out) {
      for (i32 i = 0; i < segments; i++) {
        out[static_cast<usize>(i) * 2 + 0] = {point(i), packed};
        out[static_cast<usize>(i) * 2 + 1] = {point(i + 1), packed};
      }
    }
  );
}

static auto clear_list(auto& list) -> void {
  list.line_vertices.clear();
  list.triangle_vertices.clear();
  list.texts.clear();
  list.text_chars.clear();
}

auto DebugRenderer::pack_color(const glm::vec4& color) -> u32 { return glm::packUnorm4x8(color); }

auto DebugRenderer::draw_point(
  this DebugRenderer& self,
  const glm::vec3& pos,
  const f32 point_radius,
  const glm::vec4& color,
  const bool depth_tested
) -> void {
  const auto packed = pack_color(color);
  self.emit(Primitive::Lines, 6, depth_tested, [&](std::span<Vertex> out) {
    for (i32 axis = 0; axis < 3; axis++) {
      auto offset = glm::vec3(0.0f);
      offset[axis] = point_radius;
      out[static_cast<usize>(axis) * 2 + 0] = {pos - offset, packed};
      out[static_cast<usize>(axis) * 2 + 1] = {pos + offset, packed};
    }
  });
}

auto DebugRenderer::draw_line(
  this DebugRenderer& self,
  const glm::vec3& start,
  const glm::vec3& end,
  const f32,
  const glm::vec4& color,
  const bool depth_tested
) -> void {
  const auto packed = pack_color(color);
  self.emit(Primitive::Lines, 2, depth_tested, [&](std::span<Vertex> out) {
    out[0] = {start, packed};
    out[1] = {end, packed};
  });
}

auto DebugRenderer::draw_triangle(
  this DebugRenderer& self,
  const glm::vec3& v0,
  const glm::vec3& v1,
  const glm::vec3& v2,
  const glm::vec4& color,
  const bool depth_tested
) -> void {
  const auto packed = pack_color(color);
  self.emit(Primitive::Triangles, 3, depth_tested, [&](std::span<Vertex> out) {
    out[0] = {v0, packed};
    out[1] = {v1, packed};
    out[2] = {v2, packed};
  });
}

auto DebugRenderer::draw_text(
  this DebugRenderer& self,
  const glm::vec3& position,
  std::string_view text,
  const f32 height,
  const glm::vec4& color,
  const bool depth_tested,
  const Lifetime lifetime
) -> void {
  if (text.empty())
    return;

  std::unique_lock lock(self.mutex);
  auto& list = self.draw_lists[std::to_underlying(lifetime)][depth_tested];
  list.texts.push_back(
    {.position = position,
     .height = height,
     .color = pack_color(color),
     .offset = static_cast<u32>(list.text_chars.size()),
     .length = static_cast<u32>(text.size())}
  );
  list.text_chars.append(text);
}

auto DebugRenderer::draw_circle(
  this DebugRenderer& self,
  const i32 num_verts,
  const f32 radius,
  const glm::vec3& position,
  const glm::quat& rotation,
  const glm::vec4& color,
  const bool depth_tested
) -> void {
  draw_arc(
    self,
    position,
    rotation * glm::vec3(1.0f, 0.0f, 0.0f),
    rotation * glm::vec3(0.0f, 1.0f, 0.0f),
    radius,
    0.0f,
    glm::two_pi<f32>(),
    num_verts,
    color,
    depth_tested
  );
}

auto DebugRenderer::draw_sphere(
  this DebugRenderer& self, const f32 radius, const glm::vec3& position, const glm::vec4& color, const bool depth_tested
) -> void {
  constexpr i32 NUM_VERTS = 16;
  const auto x_axis = glm::vec3(1.0f, 0.0f, 0.0f);
  const auto y_axis = glm::vec3(0.0f, 1.0f, 0.0f);

  self.draw_circle(NUM_VERTS, radius, position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), color, depth_tested);
  self.draw_circle(NUM_VERTS, radius, position, glm::angleAxis(glm::radians(90.0f), x_axis), color, depth_tested);
  self.draw_circle(NUM_VERTS, radius, position, glm::angleAxis(glm::radians(90.0f), y_axis), color, depth_tested);
  self.draw_circle(NUM_VERTS, radius, position, glm::angleAxis(glm::radians(45.0f), y_axis), color, depth_tested);
  self.draw_circle(NUM_VERTS, radius, position, glm::angleAxis(glm::radians(135.0f), y_axis), color, depth_tested);
}

auto DebugRenderer::draw_capsule(
  this DebugRenderer& self,
  const glm::vec3& position,
  const glm::quat& rotation,
  const f32 height,
  const f32 radius,
  const glm::vec4& color,
  const bool depth_tested
) -> void {
  constexpr i32 SEGMENTS = 20;
  const auto right = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
  const auto up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
  const auto forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
  const auto top = position + up * (height * 0.5f);
  const auto bottom = position - up * (height * 0.5f);
  const auto pi = glm::pi<f32>();

  draw_arc(self, top, right, forward, radius, 0.0f, 2.0f * pi, SEGMENTS, color, depth_tested);
  draw_arc(self, bottom, right, forward, radius, 0.0f, 2.0f * pi, SEGMENTS, color, depth_tested);

  for (const auto& side : {right, -right, forward, -forward}) {
    self.draw_line(bottom + side * radius, top + side * radius, 1.0f, color, depth_tested);
  }

  draw_arc(self, top, right, up, radius, 0.0f, pi, SEGMENTS / 2, color, depth_tested);
  draw_arc(self, top, forward, up, radius, 0.0f, pi, SEGMENTS / 2, color, depth_tested);
  draw_arc(self, bottom, right, -up, radius, 0.0f, pi, SEGMENTS / 2, color, depth_tested);
  draw_arc(self, bottom, forward, -up, radius, 0.0f, pi, SEGMENTS / 2, color, depth_tested);
}

auto DebugRenderer::draw_cone(
  this DebugRenderer& self,
  const i32 num_circle_verts,
  const i32 num_lines_to_circle,
  const f32 angle,
  const f32 length,
  const glm::vec3& position,
  const glm::quat& rotation,
  const glm::vec4& color,
  const bool depth_tested
) -> void {
  const auto end_radius = glm::tan(angle * 0.5f) * length;
  const auto forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
  const auto end_position = position + forward * length;
  self.draw_circle(num_circle_verts, end_radius, end_position, rotation, color, depth_tested);

  for (i32 i = 0; i < num_lines_to_circle; i++) {
    const auto a = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(num_lines_to_circle);
    const auto point = rotation * glm::vec3(glm::cos(a), glm::sin(a), 0.0f) * end_radius;
    self.draw_line(position, end_position + point, 1.0f, color, depth_tested);
  }
}

auto DebugRenderer::draw_aabb(
  this DebugRenderer& self,
  const AABB& aabb,
  const glm::vec4& color,
  const bool corners_only,
  const f32 width,
  const bool depth_tested
) -> void {
  glm::vec3 uuu = aabb.max;
  glm::vec3 lll = aabb.min;

  glm::vec3 ull(uuu.x, lll.y, lll.z);
  glm::vec3 uul(uuu.x, uuu.y, lll.z);
  glm::vec3 ulu(uuu.x, lll.y, uuu.z);

  glm::vec3 luu(lll.x, uuu.y, uuu.z);
  glm::vec3 llu(lll.x, lll.y, uuu.z);
  glm::vec3 lul(lll.x, uuu.y, lll.z);

  const std::pair<glm::vec3, glm::vec3> edges[] = {
    {luu, uuu},
    {lul, uul},
    {llu, ulu},
    {lll, ull},
    {lul, lll},
    {uul, ull},
    {luu, llu},
    {uuu, ulu},
    {lll, llu},
    {ull, ulu},
    {lul, luu},
    {uul, uuu},
  };

  for (const auto& [a, b] : edges) {
    if (!corners_only) {
      self.draw_line(a, b, width, color, depth_tested);
    } else {
      self.draw_line(a, a + (b - a) * 0.25f, width, color, depth_tested);
      self.draw_line(a + (b - a) * 0.75f, b, width, color, depth_tested);
    }
  }
}

auto DebugRenderer::draw_frustum(
  this DebugRenderer& self, const glm::mat4& frustum, const glm::vec4& color, const f32, const f32
) -> void {
  const auto inv_frustum = glm::inverse(frustum);

  // reversed z: near plane at z = 1, far plane at z = 0
  const glm::vec4 clip_corners[] = {
    glm::vec4(-1.0f, -1.0f, 1.0f, 1.0f), // bottom-left-near
    glm::vec4(1.0f, -1.0f, 1.0f, 1.0f),  // bottom-right-near
    glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f),  // top-left-near
    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),   // top-right-near
    glm::vec4(-1.0f, -1.0f, 0.0f, 1.0f), // bottom-left-far
    glm::vec4(1.0f, -1.0f, 0.0f, 1.0f),  // bottom-right-far
    glm::vec4(-1.0f, 1.0f, 0.0f, 1.0f),  // top-left-far
    glm::vec4(1.0f, 1.0f, 0.0f, 1.0f),   // top-right-far
  };

  glm::vec3 corners[8] = {};
  for (usize i = 0; i < 8; i++) {
    const auto world_pos = inv_frustum * clip_corners[i];
    corners[i] = glm::vec3(world_pos) / world_pos.w;
  }

  const auto& [bln, brn, tln, trn, blf, brf, tlf, trf] = corners;

  self.draw_line(tln, trn, 1.0f, color, false);
  self.draw_line(bln, brn, 1.0f, color, false);
  self.draw_line(tln, bln, 1.0f, color, false);
  self.draw_line(trn, brn, 1.0f, color, false);
  self.draw_line(tlf, trf, 1.0f, color, false);
  self.draw_line(blf, brf, 1.0f, color, false);
  self.draw_line(tlf, blf, 1.0f, color, false);
  self.draw_line(trf, brf, 1.0f, color, false);

  self.draw_line(tln, tlf, 1.0f, color, false);
  self.draw_line(trn, trf, 1.0f, color, false);
  self.draw_line(bln, blf, 1.0f, color, false);
  self.draw_line(brn, brf, 1.0f, color, false);
}

auto DebugRenderer::draw_ray(
  this DebugRenderer& self, const RayCast& ray, const glm::vec4& color, const f32 distance, const bool depth_tested
) -> void {
  self.draw_line(ray.get_origin(), ray.get_origin() + ray.get_direction() * distance, 1.0f, color, depth_tested);
}

auto DebugRenderer::get_view(this DebugRenderer& self) -> View {
  std::shared_lock lock(self.mutex);
  return self.view;
}

auto DebugRenderer::clear_retained(this DebugRenderer& self) -> void {
  std::unique_lock lock(self.mutex);
  for (auto& list : self.draw_lists[std::to_underlying(Lifetime::Retained)])
    clear_list(list);
}

auto DebugRenderer::flush(this DebugRenderer& self, const View& view, std::vector<Vertex>& vertices) -> DrawRanges {
  ZoneScoped;

  std::unique_lock lock(self.mutex);
  self.view = view;

  DrawRanges ranges = {};
  for (usize depth_tested = 0; depth_tested < 2; depth_tested++) {
    const auto offset = vertices.size();
    for (auto& lists : self.draw_lists) {
      const auto& list = lists[depth_tested];
      vertices.insert(vertices.end(), list.line_vertices.begin(), list.line_vertices.end());
      for (const auto& text : list.texts) {
        const auto chars = std::string_view(list.text_chars).substr(text.offset, text.length);
        append_text(vertices, view, text.position, text.height, text.color, chars);
      }
    }
    ranges.lines[depth_tested] = {static_cast<u32>(offset), static_cast<u32>(vertices.size() - offset)};
  }

  for (usize depth_tested = 0; depth_tested < 2; depth_tested++) {
    const auto offset = vertices.size();
    for (auto& lists : self.draw_lists) {
      const auto& list = lists[depth_tested];
      vertices.insert(vertices.end(), list.triangle_vertices.begin(), list.triangle_vertices.end());
    }
    ranges.triangles[depth_tested] = {static_cast<u32>(offset), static_cast<u32>(vertices.size() - offset)};
  }

  for (auto& list : self.draw_lists[std::to_underlying(Lifetime::Frame)])
    clear_list(list);

  return ranges;
}

auto DebugRenderer::discard(this DebugRenderer& self) -> void {
  std::unique_lock lock(self.mutex);
  for (auto& list : self.draw_lists[std::to_underlying(Lifetime::Frame)])
    clear_list(list);
}
} // namespace ox
