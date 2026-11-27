#include <cmath>
#include <gtest/gtest.h>
#include <numbers>

#include "UI/RmlRenderer.hpp"

using namespace ox;

constexpr auto WIDTH = 800.0f;
constexpr auto HEIGHT = 600.0f;

static auto to_ndc(const Rml::Matrix4f& transform, Rml::Vector2f pixel) -> Rml::Vector4f {
  const auto clip = rml_projection(WIDTH, HEIGHT) * transform * Rml::Vector4f(pixel.x, pixel.y, 0.0f, 1.0f);
  return {clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, clip.w};
}

TEST(RmlProjection, MapsTheContextOntoVulkanClipSpace) {
  const auto identity = Rml::Matrix4f::Identity();

  const auto top_left = to_ndc(identity, {0.0f, 0.0f});
  EXPECT_FLOAT_EQ(top_left.x, -1.0f);
  EXPECT_FLOAT_EQ(top_left.y, -1.0f);
  EXPECT_FLOAT_EQ(top_left.z, 0.5f);
  EXPECT_FLOAT_EQ(top_left.w, 1.0f);

  const auto bottom_right = to_ndc(identity, {WIDTH, HEIGHT});
  EXPECT_FLOAT_EQ(bottom_right.x, 1.0f);
  EXPECT_FLOAT_EQ(bottom_right.y, 1.0f);

  const auto center = to_ndc(identity, {WIDTH / 2.0f, HEIGHT / 2.0f});
  EXPECT_NEAR(center.x, 0.0f, 1e-6f);
  EXPECT_NEAR(center.y, 0.0f, 1e-6f);
}

// `perspective: 500px; transform: rotateY(60deg)` about the origin, a point 100px right of it recedes and shrinks
// toward it. Dropping w, as the shader used to, leaves it at exactly 100 * cos(60deg)
TEST(RmlProjection, KeepsWForThePerspectiveDivide) {
  constexpr auto distance = 500.0f;
  constexpr auto angle = std::numbers::pi_v<f32> / 3.0f;
  const auto transform = Rml::Matrix4f::Perspective(distance) * Rml::Matrix4f::RotateY(angle);

  const auto ndc = to_ndc(transform, {100.0f, 0.0f});
  EXPECT_GT(ndc.w, 1.0f);

  const auto receded_x = 100.0f * std::cos(angle) / (1.0f + 100.0f * std::sin(angle) / distance);
  EXPECT_NEAR((ndc.x + 1.0f) * WIDTH / 2.0f, receded_x, 1e-3f);
  EXPECT_LT(receded_x, 100.0f * std::cos(angle) - 1.0f);

  EXPECT_GE(ndc.z, 0.0f);
  EXPECT_LE(ndc.z, 1.0f);
}
