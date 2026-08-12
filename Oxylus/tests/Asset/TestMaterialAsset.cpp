#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Asset/AssetManager.hpp"

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

  std::unique_ptr<ox::AssetManager> asset_man = nullptr;
  std::filesystem::path directory = {};
};

TEST_F(MaterialAssetTest, ExportWritesMetaFileNextToAssetPath) {
  const auto asset_path = directory / "stone";
  const auto uuid = asset_man->create_asset(ox::AssetType::Material, asset_path);
  ASSERT_TRUE(static_cast<bool>(uuid));
  ASSERT_TRUE(asset_man->load_asset(uuid));

  EXPECT_TRUE(asset_man->export_asset(uuid, asset_path));
  EXPECT_TRUE(std::filesystem::exists(directory / "stone.oxasset"));
  EXPECT_FALSE(std::filesystem::exists(asset_path));
}

TEST_F(MaterialAssetTest, EditedPropertiesSurviveAReload) {
  const auto asset_path = directory / "stone";
  const auto albedo_texture = ox::UUID::generate_random();

  const auto uuid = asset_man->create_asset(ox::AssetType::Material, asset_path);
  ASSERT_TRUE(asset_man->load_asset(uuid));

  {
    auto material = asset_man->get_material(uuid);
    ASSERT_TRUE(static_cast<bool>(material));

    material->albedo_color = {0.25f, 0.5f, 0.75f, 0.5f};
    material->uv_size = {2.0f, 4.0f};
    material->uv_offset = {0.125f, 0.25f};
    material->emissive_color = {1.0f, 2.0f, 3.0f};
    material->roughness_factor = 0.625f;
    material->metallic_factor = 0.375f;
    material->alpha_mode = ox::AlphaMode::Mask;
    material->alpha_cutoff = 0.75f;
    material->sampling_mode = ox::SamplingMode::NearestClamped;
    material->albedo_texture = albedo_texture;
  }

  ASSERT_TRUE(asset_man->export_asset(uuid, asset_path));

  // A fresh manager stands in for restarting the editor: the material is only known through the
  // meta file that was just written.
  auto reloaded_man = std::make_unique<ox::AssetManager>();
  ASSERT_TRUE(reloaded_man->init().has_value());

  const auto reloaded_uuid = reloaded_man->register_asset(directory / "stone.oxasset");
  ASSERT_EQ(reloaded_uuid, uuid);
  ASSERT_TRUE(reloaded_man->load_asset(reloaded_uuid));

  auto material = reloaded_man->get_material(reloaded_uuid);
  ASSERT_TRUE(static_cast<bool>(material));

  EXPECT_EQ(material->albedo_color, glm::vec4(0.25f, 0.5f, 0.75f, 0.5f));
  EXPECT_EQ(material->uv_size, glm::vec2(2.0f, 4.0f));
  EXPECT_EQ(material->uv_offset, glm::vec2(0.125f, 0.25f));
  EXPECT_EQ(material->emissive_color, glm::vec3(1.0f, 2.0f, 3.0f));
  EXPECT_FLOAT_EQ(material->roughness_factor, 0.625f);
  EXPECT_FLOAT_EQ(material->metallic_factor, 0.375f);
  EXPECT_EQ(material->alpha_mode, ox::AlphaMode::Mask);
  EXPECT_FLOAT_EQ(material->alpha_cutoff, 0.75f);
  EXPECT_EQ(material->sampling_mode, ox::SamplingMode::NearestClamped);
  EXPECT_EQ(material->albedo_texture, albedo_texture);

  material.reset();

  auto deinit_result = reloaded_man->deinit();
  EXPECT_TRUE(deinit_result.has_value());
}

TEST_F(MaterialAssetTest, LoadingAMaterialWithoutAMetaFileFallsBackToDefaults) {
  const auto uuid = asset_man->create_asset(ox::AssetType::Material, directory / "missing");
  ASSERT_TRUE(asset_man->load_asset(uuid));

  auto material = asset_man->get_material(uuid);
  ASSERT_TRUE(static_cast<bool>(material));
  EXPECT_EQ(material->albedo_color, ox::Material{}.albedo_color);
  EXPECT_EQ(material->alpha_mode, ox::Material{}.alpha_mode);
}
