#include "Scripting/LuaMathBindings.hpp"

#include <sol/overload.hpp>
#include <sol/state.hpp>
#include <sol/types.hpp>

#include "Scripting/LuaHelpers.hpp"
#include "Render/BoundingVolume.hpp"

namespace ox::LuaBindings {
#define SET_MATH_FUNCTIONS(var, type, number)                                                                    \
  (var).set_function(sol::meta_function::division, [](const type& a, const type& b) { return a / b; });          \
  (var).set_function(sol::meta_function::equal_to, [](const type& a, const type& b) { return a == b; });         \
  (var).set_function(sol::meta_function::unary_minus, [](const type& v) -> type { return -v; });                 \
  (var).set_function(sol::meta_function::multiplication, sol::overload([](const type& a, const type& b) {        \
    return a * b;                                                                                                \
  }, [](const type& a, const number b) { return a * b; }, [](const number a, const type& b) { return a * b; })); \
  (var).set_function(sol::meta_function::subtraction, sol::overload([](const type& a, const type& b) {           \
    return a - b;                                                                                                \
  }, [](const type& a, const number b) { return a - b; }, [](const number a, const type& b) { return a - b; })); \
  (var).set_function(sol::meta_function::addition, sol::overload([](const type& a, const type& b) {              \
    return a + b;                                                                                                \
  }, [](const type& a, const number b) { return a + b; }, [](const number a, const type& b) { return a + b; }));

void bind_math(sol::state* state) {
  ZoneScoped;
  auto vec2 = state->new_usertype<glm::vec2>("Vec2", sol::constructors<glm::vec2(float, float), glm::vec2(float)>());
  SET_TYPE_FIELD(vec2, glm::vec2, x);
  SET_TYPE_FIELD(vec2, glm::vec2, y);
  SET_MATH_FUNCTIONS(vec2, glm::vec2, float)

  auto uvec2 = state->new_usertype<glm::uvec2>(
      "UVec2", sol::constructors<glm::uvec2(uint32_t, uint32_t), glm::uvec2(uint32_t)>());
  SET_TYPE_FIELD(uvec2, glm::uvec2, x);
  SET_TYPE_FIELD(uvec2, glm::uvec2, y);
  SET_MATH_FUNCTIONS(uvec2, glm::uvec2, uint32_t)

  auto ivec2 = state->new_usertype<glm::ivec2>("IVec2", sol::constructors<glm::ivec2(int, int), glm::ivec2(int)>());
  SET_TYPE_FIELD(ivec2, glm::ivec2, x);
  SET_TYPE_FIELD(ivec2, glm::ivec2, y);
  SET_MATH_FUNCTIONS(ivec2, glm::ivec2, int)

  auto vec3 = state->new_usertype<glm::vec3>(
      "Vec3", sol::constructors<sol::types<>, sol::types<float, float, float>, glm::vec3(float)>());
  SET_TYPE_FIELD(vec3, glm::vec3, x);
  SET_TYPE_FIELD(vec3, glm::vec3, y);
  SET_TYPE_FIELD(vec3, glm::vec3, z);
  SET_MATH_FUNCTIONS(vec3, glm::vec3, float)

  auto ivec3 = state->new_usertype<glm::ivec3>(
      "IVec3", sol::constructors<sol::types<>, sol::types<int, int, int>, glm::ivec3(int)>());
  SET_TYPE_FIELD(ivec3, glm::ivec3, x);
  SET_TYPE_FIELD(ivec3, glm::ivec3, y);
  SET_TYPE_FIELD(ivec3, glm::ivec3, z);
  SET_MATH_FUNCTIONS(ivec3, glm::ivec3, int)

  auto uvec3 = state->new_usertype<glm::uvec3>(
      "UVec3", sol::constructors<sol::types<>, sol::types<uint32_t, uint32_t, uint32_t>, glm::uvec3(uint32_t)>());
  SET_TYPE_FIELD(uvec3, glm::uvec3, x);
  SET_TYPE_FIELD(uvec3, glm::uvec3, y);
  SET_TYPE_FIELD(uvec3, glm::uvec3, z);
  SET_MATH_FUNCTIONS(uvec3, glm::uvec3, uint32_t)

  auto vec4 = state->new_usertype<glm::vec4>(
      "Vec4", sol::constructors<sol::types<>, sol::types<float, float, float, float>, glm::vec4(float)>());
  SET_TYPE_FIELD(vec4, glm::vec4, x);
  SET_TYPE_FIELD(vec4, glm::vec4, y);
  SET_TYPE_FIELD(vec4, glm::vec4, z);
  SET_TYPE_FIELD(vec4, glm::vec4, w);
  SET_MATH_FUNCTIONS(vec4, glm::vec4, float)

  auto ivec4 = state->new_usertype<glm::ivec4>(
      "IVec4", sol::constructors<sol::types<>, sol::types<int, int, int, int>, glm::ivec4(int)>());
  SET_TYPE_FIELD(ivec4, glm::ivec4, x);
  SET_TYPE_FIELD(ivec4, glm::ivec4, y);
  SET_TYPE_FIELD(ivec4, glm::ivec4, z);
  SET_TYPE_FIELD(ivec4, glm::ivec4, w);
  SET_MATH_FUNCTIONS(ivec4, glm::ivec4, int)

  auto uvec4 = state->new_usertype<glm::uvec4>(
      "UVec4",
      sol::constructors<sol::types<>, sol::types<uint32_t, uint32_t, uint32_t, uint32_t>, glm::uvec4(uint32_t)>());
  SET_TYPE_FIELD(uvec4, glm::uvec4, x);
  SET_TYPE_FIELD(uvec4, glm::uvec4, y);
  SET_TYPE_FIELD(uvec4, glm::uvec4, z);
  SET_TYPE_FIELD(uvec4, glm::uvec4, w);
  SET_MATH_FUNCTIONS(uvec4, glm::uvec4, uint32_t)

  state->new_usertype<glm::mat3>(
      "Mat3",
      sol::constructors<glm::mat3(float, float, float, float, float, float, float, float, float),
                        glm::mat3(float),
                        glm::mat3()>(),
      sol::meta_function::multiplication,
      [](const glm::mat3& a, const glm::mat3& b) { return a * b; });

  state->new_usertype<glm::mat4>("Mat4",
                                 sol::constructors<glm::mat4(float), glm::mat4()>(),
                                 sol::meta_function::multiplication,
                                 [](const glm::mat4& a, const glm::mat4& b) { return a * b; },
                                 sol::meta_function::addition,
                                 [](const glm::mat4& a, const glm::mat4& b) { return a + b; },
                                 sol::meta_function::subtraction,
                                 [](const glm::mat4& a, const glm::mat4& b) { return a - b; });

  state->new_enum<Intersection>("Intersection", {{"Outside", Outside}, {"Intersects", Intersects}, {"Inside", Inside}});

  auto aabb = state->new_usertype<AABB>("AABB", sol::constructors<AABB(), AABB(AABB), AABB(glm::vec3, glm::vec3)>());
  SET_TYPE_FIELD(aabb, AABB, min);
  SET_TYPE_FIELD(aabb, AABB, max);
  SET_TYPE_FUNCTION(aabb, AABB, get_center);
  SET_TYPE_FUNCTION(aabb, AABB, get_extents);
  SET_TYPE_FUNCTION(aabb, AABB, get_size);
  SET_TYPE_FUNCTION(aabb, AABB, translate);
  SET_TYPE_FUNCTION(aabb, AABB, scale);
  SET_TYPE_FUNCTION(aabb, AABB, transform);
  SET_TYPE_FUNCTION(aabb, AABB, get_transformed);
  SET_TYPE_FUNCTION(aabb, AABB, merge);
  // TODO: Bind frustum and Plane
  // SET_TYPE_FUNCTION(aabb, AABB, is_on_frustum);
  // SET_TYPE_FUNCTION(aabb, AABB, is_on_or_forward_plane);
  aabb.set_function("intersects", sol::overload([](const AABB& self, const glm::vec3& point) {
    return self.intersects(point);
  }, [](const AABB& self, const AABB& box) { return self.intersects(box); }));

  // --- glm functions ---
  auto glm_table = state->create_table("glm");
  glm_table.set_function("translate",
                         [](const glm::mat4& mat, const glm::vec3& vec) { return glm::translate(mat, vec); });

  glm_table.set_function("floor", sol::overload([](const float v) { return glm::floor(v); }, [](const glm::vec2& vec) {
    return glm::floor(vec);
  }, [](const glm::vec3& vec) { return glm::floor(vec); }, [](const glm::vec4& vec) { return glm::floor(vec); }));
  glm_table.set_function("ceil", sol::overload([](const float v) { return glm::ceil(v); }, [](const glm::vec2& vec) {
    return glm::ceil(vec);
  }, [](const glm::vec3& vec) { return glm::ceil(vec); }, [](const glm::vec4& vec) { return glm::ceil(vec); }));
  glm_table.set_function("round", sol::overload([](const float v) { return glm::round(v); }, [](const glm::vec2& vec) {
    return glm::round(vec);
  }, [](const glm::vec3& vec) { return glm::round(vec); }, [](const glm::vec4& vec) { return glm::round(vec); }));
  glm_table.set_function("length", sol::overload([](const glm::vec2& vec) {
    return glm::length(vec);
  }, [](const glm::vec3& vec) { return glm::length(vec); }, [](const glm::vec4& vec) { return glm::length(vec); }));
  glm_table.set_function(
      "normalize", sol::overload([](const glm::vec2& vec) { return glm::normalize(vec); }, [](const glm::vec3& vec) {
    return glm::normalize(vec);
  }, [](const glm::vec4& vec) { return glm::normalize(vec); }));
  glm_table.set_function("distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::distance(a, b); });
}
} // namespace ox::LuaBindings
