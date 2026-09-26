#include "Animation/BoneTransform.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace ox {
auto BoneTransform::from_mat4(const glm::mat4& mat) -> BoneTransform {
  ZoneScoped;

  auto scale = glm::vec3(
    glm::length(glm::vec3(mat[0])),
    glm::length(glm::vec3(mat[1])),
    glm::length(glm::vec3(mat[2]))
  );

  auto rotation_mat = glm::mat3(mat);
  if (scale.x > glm::epsilon<f32>())
    rotation_mat[0] /= scale.x;
  if (scale.y > glm::epsilon<f32>())
    rotation_mat[1] /= scale.y;
  if (scale.z > glm::epsilon<f32>())
    rotation_mat[2] /= scale.z;

  // a mirrored basis cannot be expressed as quaternion plus positive scale, so flipping the whole
  // basis keeps the rotation proper and lands the sign on the scale instead
  auto uniform_scale = (scale.x + scale.y + scale.z) / 3.f;
  if (glm::determinant(rotation_mat) < 0.f) {
    rotation_mat = -rotation_mat;
    uniform_scale = -uniform_scale;
  }

  return BoneTransform{
    .rotation = glm::normalize(glm::quat_cast(rotation_mat)),
    .translation_scale = glm::vec4(glm::vec3(mat[3]), uniform_scale),
  };
}

auto BoneTransform::inverse(this const BoneTransform& self) -> BoneTransform {
  const auto inv_rotation = glm::conjugate(self.rotation);
  const auto scale = self.translation_scale.w;
  const auto inv_scale = glm::abs(scale) > glm::epsilon<f32>() ? 1.f / scale : 0.f;

  return BoneTransform{
    .rotation = inv_rotation,
    .translation_scale = glm::vec4(inv_rotation * -self.translation() * inv_scale, inv_scale),
  };
}

auto operator*(const BoneTransform& a, const BoneTransform& b) -> BoneTransform {
  return BoneTransform{
    .rotation = a.rotation * b.rotation,
    .translation_scale = glm::vec4(
      a.rotation * (b.translation() * a.translation_scale.w) + a.translation(),
      a.translation_scale.w * b.translation_scale.w
    ),
  };
}

auto slerp(const BoneTransform& a, const BoneTransform& b, const f32 t) -> BoneTransform {
  return BoneTransform{
    .rotation = glm::slerp(a.rotation, b.rotation, t),
    .translation_scale = glm::mix(a.translation_scale, b.translation_scale, t),
  };
}
} // namespace ox
