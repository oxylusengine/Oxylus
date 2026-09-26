#pragma once

#include <fastgltf/tools.hpp>
#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

template <>
struct fastgltf::ElementTraits<glm::vec4> : fastgltf::ElementTraitsBase<glm::vec4, AccessorType::Vec4, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec3> : fastgltf::ElementTraitsBase<glm::vec3, AccessorType::Vec3, float> {};
template <>
struct fastgltf::ElementTraits<glm::vec2> : fastgltf::ElementTraitsBase<glm::vec2, AccessorType::Vec2, float> {};
template <>
struct fastgltf::ElementTraits<glm::u16vec4>
    : fastgltf::ElementTraitsBase<glm::u16vec4, AccessorType::Vec4, std::uint16_t> {};
