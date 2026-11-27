#include <flecs.h>
#include <gtest/gtest.h>

#include "Asset/AssetHandle.hpp"
#include "Scene/Components.hpp"

TEST(DecalComponentTest, SurvivesReflectionJsonRoundTrip) {
  auto world = flecs::world{};
  world.import<ox::CoreComponentsModule>();

  const auto source = ox::DecalComponent{
    .material = ox::AssetHandle<ox::Material>::acquire(
      ox::UUID::from_string("2b0c8f3e-6c51-4c55-9f6f-3c1d8a0e7b21").value()
    ),
    .opacity = 0.35f,
    .sort_order = -7,
    .angle_fade_start = 30.0f,
    .angle_fade_end = 55.0f,
    .depth_fade = 0.25f,
    .layer_mask = 0b1010,
    .affect_albedo = false,
    .affect_normal = false,
    .affect_metallic_roughness = false,
    .affect_emissive = true,
  };

  const auto json = world.to_json(&source);
  ASSERT_NE(json.c_str(), nullptr);

  auto decal = ox::DecalComponent{};
  ASSERT_NE(world.from_json(&decal, json.c_str()), nullptr) << json.c_str();

  EXPECT_EQ(decal.material.uuid(), source.material.uuid());
  EXPECT_FLOAT_EQ(decal.opacity, 0.35f);
  EXPECT_EQ(decal.sort_order, -7);
  EXPECT_FLOAT_EQ(decal.angle_fade_start, 30.0f);
  EXPECT_FLOAT_EQ(decal.angle_fade_end, 55.0f);
  EXPECT_FLOAT_EQ(decal.depth_fade, 0.25f);
  EXPECT_EQ(decal.layer_mask, 0b1010);
  EXPECT_FALSE(decal.affect_albedo);
  EXPECT_FALSE(decal.affect_normal);
  EXPECT_FALSE(decal.affect_metallic_roughness);
  EXPECT_TRUE(decal.affect_emissive);
}
