#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Asset/AssetManager.hpp"
#include "Utils/Log.hpp"

// The `.oxasset` sidecar lives in OxylusEditor now, and test targets link only Oxylus, so what is
// covered here is the engine half of that split: the pending load-info map an editor pushes into.
class MaterialAssetTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_WARNING;

    directory = std::filesystem::temp_directory_path() / "ox_material_asset_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    asset_man = std::make_unique<ox::AssetManager>();
    ASSERT_TRUE(asset_man->init().has_value());
  }

  void TearDown() override {
    auto deinit_result = asset_man->deinit();
    EXPECT_TRUE(deinit_result.has_value());
    asset_man.reset();

    std::filesystem::remove_all(directory);
  }

  auto edited_material(const ox::UUID& albedo_texture) const -> ox::Material {
    auto material = ox::Material{};
    material.albedo_color = {0.25f, 0.5f, 0.75f, 0.5f};
    material.uv_size = {2.0f, 4.0f};
    material.uv_offset = {0.125f, 0.25f};
    material.emissive_color = {1.0f, 2.0f, 3.0f};
    material.roughness_factor = 0.625f;
    material.metallic_factor = 0.375f;
    material.alpha_mode = ox::AlphaMode::Mask;
    material.alpha_cutoff = 0.75f;
    material.sampling_mode = ox::SamplingMode::NearestClamped;
    material.normal_scale = 0.5f;
    material.occlusion_strength = 0.25f;
    material.flip_normal_y = true;
    material.double_sided = true;
    material.transmission_factor = 0.875f;
    material.ior = 1.25f;
    material.thickness_factor = 2.5f;
    material.attenuation_color = {0.5f, 0.25f, 0.125f};
    material.attenuation_distance = 8.0f;
    material.shading_model = ox::ShadingModel::Water;
    material.water_scatter_color = {0.0625f, 0.125f, 0.1875f};
    material.water_flow = {0.5f, -0.25f};
    material.water_detail_scale = 0.75f;
    material.water_shore_fade = 1.5f;
    material.albedo_texture = albedo_texture;
    material.normal_texture = extra_textures[0];
    material.emissive_texture = extra_textures[1];
    material.metallic_roughness_texture = extra_textures[2];
    material.occlusion_texture = extra_textures[3];
    material.transmission_texture = extra_textures[4];
    material.thickness_texture = extra_textures[5];

    return material;
  }

  auto expect_matches_edited(const ox::Material& material, const ox::UUID& albedo_texture) const -> void {
    EXPECT_EQ(material.albedo_color, glm::vec4(0.25f, 0.5f, 0.75f, 0.5f));
    EXPECT_EQ(material.uv_size, glm::vec2(2.0f, 4.0f));
    EXPECT_EQ(material.uv_offset, glm::vec2(0.125f, 0.25f));
    EXPECT_EQ(material.emissive_color, glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_FLOAT_EQ(material.roughness_factor, 0.625f);
    EXPECT_FLOAT_EQ(material.metallic_factor, 0.375f);
    EXPECT_EQ(material.alpha_mode, ox::AlphaMode::Mask);
    EXPECT_FLOAT_EQ(material.alpha_cutoff, 0.75f);
    EXPECT_EQ(material.sampling_mode, ox::SamplingMode::NearestClamped);
    EXPECT_FLOAT_EQ(material.normal_scale, 0.5f);
    EXPECT_FLOAT_EQ(material.occlusion_strength, 0.25f);
    EXPECT_TRUE(material.flip_normal_y);
    EXPECT_TRUE(material.double_sided);
    EXPECT_FLOAT_EQ(material.transmission_factor, 0.875f);
    EXPECT_FLOAT_EQ(material.ior, 1.25f);
    EXPECT_FLOAT_EQ(material.thickness_factor, 2.5f);
    EXPECT_EQ(material.attenuation_color, glm::vec3(0.5f, 0.25f, 0.125f));
    EXPECT_FLOAT_EQ(material.attenuation_distance, 8.0f);
    EXPECT_EQ(material.shading_model, ox::ShadingModel::Water);
    EXPECT_EQ(material.water_scatter_color, glm::vec3(0.0625f, 0.125f, 0.1875f));
    EXPECT_EQ(material.water_flow, glm::vec2(0.5f, -0.25f));
    EXPECT_FLOAT_EQ(material.water_detail_scale, 0.75f);
    EXPECT_FLOAT_EQ(material.water_shore_fade, 1.5f);
    EXPECT_EQ(material.albedo_texture, albedo_texture);
    EXPECT_EQ(material.normal_texture, extra_textures[0]);
    EXPECT_EQ(material.emissive_texture, extra_textures[1]);
    EXPECT_EQ(material.metallic_roughness_texture, extra_textures[2]);
    EXPECT_EQ(material.occlusion_texture, extra_textures[3]);
    EXPECT_EQ(material.transmission_texture, extra_textures[4]);
    EXPECT_EQ(material.thickness_texture, extra_textures[5]);
  }

  std::array<ox::UUID, 6> extra_textures = {
    ox::UUID::generate_random(),
    ox::UUID::generate_random(),
    ox::UUID::generate_random(),
    ox::UUID::generate_random(),
    ox::UUID::generate_random(),
    ox::UUID::generate_random(),
  };
  std::unique_ptr<ox::AssetManager> asset_man = nullptr;
  std::filesystem::path directory = {};
};

TEST_F(MaterialAssetTest, PendingLoadInfoReachesALazyLoad) {
  const auto albedo_texture = ox::UUID::generate_random();
  const auto uuid = asset_man->create_asset(ox::AssetType::Material, directory / "stone");
  ASSERT_TRUE(static_cast<bool>(uuid));

  asset_man->set_pending_load_info(uuid, edited_material(albedo_texture));

  // no info at the call site: the load has to find it in the pending map on its own
  ASSERT_TRUE(asset_man->load_asset(uuid));

  auto material = asset_man->get_material(uuid);
  ASSERT_TRUE(static_cast<bool>(material));
  expect_matches_edited(*material.value, albedo_texture);
}

TEST_F(MaterialAssetTest, ExplicitLoadInfoWinsOverThePendingEntry) {
  const auto uuid = asset_man->create_asset(ox::AssetType::Material, directory / "stone");
  asset_man->set_pending_load_info(uuid, edited_material(ox::UUID::generate_random()));

  auto explicit_material = ox::Material{};
  explicit_material.roughness_factor = 0.125f;
  ASSERT_TRUE(asset_man->load_asset(uuid, explicit_material));

  auto material = asset_man->get_material(uuid);
  ASSERT_TRUE(static_cast<bool>(material));
  EXPECT_FLOAT_EQ(material->roughness_factor, 0.125f);
}

// Dropping the last reference erases the registry entry too, so a reload goes through
// `register_asset` again -- which is what the editor's project scan does. The pending entry has to
// outlive that, since only `delete_asset` is meant to clear it.
TEST_F(MaterialAssetTest, PendingEntrySurvivesAnUnloadAndReRegister) {
  const auto albedo_texture = ox::UUID::generate_random();
  const auto asset_path = directory / "stone";
  const auto uuid = asset_man->create_asset(ox::AssetType::Material, asset_path);
  asset_man->set_pending_load_info(uuid, edited_material(albedo_texture));

  ASSERT_TRUE(asset_man->load_asset(uuid));
  asset_man->unload_asset(uuid);
  ASSERT_FALSE(asset_man->is_loaded(uuid));

  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Material, asset_path));
  ASSERT_TRUE(asset_man->load_asset(uuid));

  auto material = asset_man->get_material(uuid);
  ASSERT_TRUE(static_cast<bool>(material));
  expect_matches_edited(*material.value, albedo_texture);
}

TEST_F(MaterialAssetTest, LoadingAMaterialWithoutPendingInfoFallsBackToDefaults) {
  const auto uuid = asset_man->create_asset(ox::AssetType::Material, directory / "missing");
  ASSERT_TRUE(asset_man->load_asset(uuid));

  auto material = asset_man->get_material(uuid);
  ASSERT_TRUE(static_cast<bool>(material));
  EXPECT_EQ(material->albedo_color, ox::Material{}.albedo_color);
  EXPECT_EQ(material->alpha_mode, ox::Material{}.alpha_mode);
}

TEST_F(MaterialAssetTest, DeletingAnAssetClearsItsPendingEntry) {
  const auto albedo_texture = ox::UUID::generate_random();
  const auto asset_path = directory / "stone";
  const auto uuid = asset_man->create_asset(ox::AssetType::Material, asset_path);
  asset_man->set_pending_load_info(uuid, edited_material(albedo_texture));

  ASSERT_TRUE(asset_man->load_asset(uuid));
  asset_man->delete_asset(uuid);

  // the same UUID registered again must not inherit the deleted asset's pending material
  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Material, asset_path));
  ASSERT_TRUE(asset_man->load_asset(uuid));

  auto material = asset_man->get_material(uuid);
  ASSERT_TRUE(static_cast<bool>(material));
  EXPECT_EQ(material->albedo_color, ox::Material{}.albedo_color);
  EXPECT_NE(material->albedo_texture, albedo_texture);
}
