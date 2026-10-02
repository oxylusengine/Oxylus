#include <gtest/gtest.h>

#include "Asset/AssetManager.hpp"
#include "Utils/Log.hpp"

// runs without an app, so nothing is mounted and the registry keeps physical paths
class AssetLookupTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;

    directory = std::filesystem::temp_directory_path() / "ox_asset_lookup_test";
    asset_man = std::make_unique<ox::AssetManager>();
    ASSERT_TRUE(asset_man->init().has_value());
  }

  void TearDown() override {
    EXPECT_TRUE(asset_man->deinit().has_value());
    asset_man.reset();
  }

  std::unique_ptr<ox::AssetManager> asset_man = nullptr;
  std::filesystem::path directory = {};
};

TEST_F(AssetLookupTest, FindsACookedAssetByItsSourceFile) {
  const auto uuid = ox::UUID::generate_random();
  ASSERT_TRUE(
    asset_man->register_asset(uuid, ox::AssetType::Model, directory / "cache" / "car.oxpack", directory / "car.glb")
  );

  EXPECT_EQ(asset_man->find_asset(directory / "car.glb"), uuid);
  EXPECT_EQ(asset_man->find_asset(directory / "sub" / ".." / "car.glb"), uuid);
  EXPECT_FALSE(asset_man->find_asset(directory / "cache" / "car.oxpack"));
}

TEST_F(AssetLookupTest, FindsAnAssetCreatedFromItsFile) {
  const auto uuid = asset_man->create_asset(ox::AssetType::Audio, directory / "engine.wav");
  EXPECT_EQ(asset_man->find_asset(directory / "engine.wav"), uuid);
}

TEST_F(AssetLookupTest, AssetsWithoutASourceAreNotFindable) {
  const auto uuid = ox::UUID::generate_random();
  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Material, directory / "car.glb"));
  EXPECT_FALSE(asset_man->find_asset(directory / "car.glb"));
}

TEST_F(AssetLookupTest, TheNewestImportOfASourceWins) {
  const auto first = ox::UUID::generate_random();
  const auto second = ox::UUID::generate_random();
  ASSERT_TRUE(asset_man->register_asset(first, ox::AssetType::Texture, directory / "a.oxpack", directory / "a.png"));
  ASSERT_TRUE(asset_man->register_asset(second, ox::AssetType::Texture, directory / "b.oxpack", directory / "a.png"));
  EXPECT_EQ(asset_man->find_asset(directory / "a.png"), second);
}

TEST_F(AssetLookupTest, UpdatingThePathMovesTheLookup) {
  const auto uuid = ox::UUID::generate_random();
  ASSERT_TRUE(asset_man->register_asset(uuid, ox::AssetType::Model, directory / "car.oxpack", directory / "car.glb"));
  ASSERT_TRUE(asset_man->update_asset_path(uuid, directory / "car.oxpack", directory / "Cars" / "car.glb"));

  EXPECT_FALSE(asset_man->find_asset(directory / "car.glb"));
  EXPECT_EQ(asset_man->find_asset(directory / "Cars" / "car.glb"), uuid);
}

TEST_F(AssetLookupTest, DeletingAnAssetDropsItsLookup) {
  const auto uuid = asset_man->create_asset(ox::AssetType::Audio, directory / "engine.wav");
  asset_man->delete_asset(uuid);
  EXPECT_FALSE(asset_man->find_asset(directory / "engine.wav"));
}
