#pragma once

#include <glm/gtx/quaternion.hpp>

#include "Animation/Fwd.hpp"

namespace ox {
// uniform scale only, carried in `w` of `translation_scale`, which keeps this at 32 bytes and lets
// a blend lerp translation and scale in one operation
struct BoneTransform {
  glm::quat rotation = glm::quat::wxyz(1.f, 0.f, 0.f, 0.f);
  glm::vec4 translation_scale = {0.f, 0.f, 0.f, 1.f};

  static auto from_trs(const glm::vec3& translation, const glm::quat& rotation, f32 scale) -> BoneTransform {
    return BoneTransform{
      .rotation = rotation,
      .translation_scale = glm::vec4(translation, scale),
    };
  }

  static auto from_mat4(const glm::mat4& mat) -> BoneTransform;

  auto translation(this const BoneTransform& self) -> glm::vec3 { return glm::vec3(self.translation_scale); }
  auto scale(this const BoneTransform& self) -> f32 { return self.translation_scale.w; }

  auto transform_point(this const BoneTransform& self, const glm::vec3& point) -> glm::vec3 {
    return self.rotation * (point * self.translation_scale.w) + self.translation();
  }

  auto transform_normal(this const BoneTransform& self, const glm::vec3& normal) -> glm::vec3 {
    return self.rotation * normal;
  }

  auto to_mat4(this const BoneTransform& self) -> glm::mat4 {
    auto mat = glm::toMat4(self.rotation);
    mat[0] *= self.translation_scale.w;
    mat[1] *= self.translation_scale.w;
    mat[2] *= self.translation_scale.w;
    mat[3] = glm::vec4(self.translation(), 1.f);
    return mat;
  }

  auto inverse(this const BoneTransform& self) -> BoneTransform;

  auto normalized(this const BoneTransform& self) -> BoneTransform {
    return BoneTransform{
      .rotation = glm::normalize(self.rotation),
      .translation_scale = self.translation_scale,
    };
  }
};

// `(a * b).to_mat4() == a.to_mat4() * b.to_mat4()`
auto operator*(const BoneTransform& a, const BoneTransform& b) -> BoneTransform;

auto slerp(const BoneTransform& a, const BoneTransform& b, f32 t) -> BoneTransform;
} // namespace ox
