#include <flecs.h>
#include <gtest/gtest.h>

#include "Scene/Components.hpp"

static auto expect_layer_eq(const ox::DensityProfileLayer& actual, const ox::DensityProfileLayer& expected) -> void {
  EXPECT_FLOAT_EQ(actual.width, expected.width);
  EXPECT_FLOAT_EQ(actual.exp_term, expected.exp_term);
  EXPECT_FLOAT_EQ(actual.exp_scale, expected.exp_scale);
  EXPECT_FLOAT_EQ(actual.linear_term, expected.linear_term);
  EXPECT_FLOAT_EQ(actual.constant_term, expected.constant_term);
}

TEST(AtmosphereComponentTest, SurvivesReflectionJsonRoundTrip) {
  auto world = flecs::world{};
  world.import<ox::CoreComponentsModule>();

  auto source = ox::AtmosphereComponent{};
  source.base_altitude_km = 2.5f;
  source.ozone_layer_1 = {
    .width = 41.0f,
    .exp_term = 3.25f,
    .exp_scale = -0.125f,
    .linear_term = 0.0625f,
    .constant_term = 0.5f,
  };
  source.mie_layer_0 = {
    .width = 9.0f,
    .exp_term = 1.5f,
    .exp_scale = -0.75f,
    .linear_term = -0.03125f,
    .constant_term = 0.25f,
  };

  const auto json = world.to_json(&source);
  ASSERT_NE(json.c_str(), nullptr);

  auto atmosphere = ox::AtmosphereComponent{};
  atmosphere.base_altitude_km = 0.0f;
  atmosphere.ozone_layer_1 = {};
  atmosphere.mie_layer_0 = {};
  ASSERT_NE(world.from_json(&atmosphere, json.c_str()), nullptr) << json.c_str();

  EXPECT_FLOAT_EQ(atmosphere.base_altitude_km, 2.5f);
  expect_layer_eq(atmosphere.ozone_layer_1, source.ozone_layer_1);
  expect_layer_eq(atmosphere.mie_layer_0, source.mie_layer_0);
  expect_layer_eq(atmosphere.rayleigh_layer_0, source.rayleigh_layer_0);
  expect_layer_eq(atmosphere.rayleigh_layer_1, source.rayleigh_layer_1);
  expect_layer_eq(atmosphere.mie_layer_1, source.mie_layer_1);
  expect_layer_eq(atmosphere.ozone_layer_0, source.ozone_layer_0);
}
