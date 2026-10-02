#pragma once

// read by both C++ and Slang, see shared.slang. declares no Slang types so it can be included twice

#ifdef __cplusplus
  #include <glm/ext/vector_int2_sized.hpp>
  #include <glm/ext/vector_uint2_sized.hpp>
  #include <glm/ext/vector_uint3_sized.hpp>
  #include <glm/ext/vector_uint4_sized.hpp>
  #include <glm/mat4x4.hpp>
  #include <glm/vec2.hpp>
  #include <glm/vec3.hpp>
  #include <glm/vec4.hpp>

  #include "Core/Enum.hpp"
  #include "Core/Types.hpp"

namespace ox::GPU {
using f32x2 = glm::vec2;
using f32x3 = glm::vec3;
using f32x4 = glm::vec4;
using i32x2 = glm::ivec2;
using i32x3 = glm::ivec3;
using i32x4 = glm::ivec4;
using u32x2 = glm::uvec2;
using u32x3 = glm::uvec3;
using u32x4 = glm::uvec4;
using u16x2 = glm::u16vec2;
using u16x3 = glm::u16vec3;
using u16x4 = glm::u16vec4;
using i8x2 = glm::i8vec2;
using mat4 = glm::mat4;

// slang lays these out with scalar rules, an over-aligned glm build would silently shift every field
template <typename T, usize Size, usize Align>
constexpr bool IS_SCALAR_LAYOUT = sizeof(T) == Size && alignof(T) == Align;
static_assert(IS_SCALAR_LAYOUT<f32x2, 8, 4>);
static_assert(IS_SCALAR_LAYOUT<f32x3, 12, 4>);
static_assert(IS_SCALAR_LAYOUT<f32x4, 16, 4>);
static_assert(IS_SCALAR_LAYOUT<i32x2, 8, 4>);
static_assert(IS_SCALAR_LAYOUT<i32x3, 12, 4>);
static_assert(IS_SCALAR_LAYOUT<i32x4, 16, 4>);
static_assert(IS_SCALAR_LAYOUT<u32x2, 8, 4>);
static_assert(IS_SCALAR_LAYOUT<u32x3, 12, 4>);
static_assert(IS_SCALAR_LAYOUT<u32x4, 16, 4>);
static_assert(IS_SCALAR_LAYOUT<u16x2, 4, 2>);
static_assert(IS_SCALAR_LAYOUT<u16x3, 6, 2>);
static_assert(IS_SCALAR_LAYOUT<u16x4, 8, 2>);
static_assert(IS_SCALAR_LAYOUT<i8x2, 2, 1>);
static_assert(IS_SCALAR_LAYOUT<mat4, 64, 4>);
} // namespace ox::GPU

  #define OX_GPU_BEGIN namespace ox::GPU {
  #define OX_GPU_END }
  #define OX_CONST constexpr static
  #define OX_BITMASK(name) consteval void enable_bitmask(name);
  #define OX_PTR(type) u64
#else
  #define OX_GPU_BEGIN
  #define OX_GPU_END
  #define OX_CONST static const
  #define OX_BITMASK(name)
  #define OX_PTR(type) type*
#endif
