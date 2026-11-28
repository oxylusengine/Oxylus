#include <gtest/gtest.h>
#include <thread>

#include "Asset/AssetHandle.hpp"
#include "Asset/AssetManager.hpp"
#include "Asset/ParticleSystem.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/App.hpp"
#include "Utils/Log.hpp"

// loads that run on a plain thread are staged, because JobManager::is_main_thread is false there
class AssetStagingTest : public ::testing::Test {
protected:
  void SetUp() override {
    static char arg0[] = "TestAssetStaging";
    static char* argv[] = {arg0, nullptr};
    app = std::make_unique<ox::App>(1, argv);
    // registered for the dependency check only, nothing here plays audio
    app->with<ox::AudioEngine>().with<ox::AssetManager>();
    loguru::g_stderr_verbosity = loguru::Verbosity_WARNING;

    directory = std::filesystem::temp_directory_path() / "ox_asset_staging_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    ASSERT_TRUE(asset_man().init().has_value());
  }

  void TearDown() override {
    EXPECT_TRUE(asset_man().deinit().has_value());
    app.reset();
    std::filesystem::remove_all(directory);
  }

  static auto asset_man() -> ox::AssetManager& { return ox::App::mod<ox::AssetManager>(); }

  static auto on_worker(auto&& fn) -> void {
    auto worker = std::thread(std::forward<decltype(fn)>(fn));
    worker.join();
  }

  static auto ref_count(const ox::UUID& uuid) -> u64 {
    const auto asset = asset_man().get_asset(uuid);
    return asset ? asset->ref_count : ~0_u64;
  }

  auto particle_system_with(const ox::UUID& material) const -> ox::UUID {
    auto system = ox::ParticleSystem{};
    system.render.material = material;
    const auto path = directory / "sparks.oxparticle";
    EXPECT_TRUE(system.write(path));
    return asset_man().create_asset(ox::AssetType::ParticleSystem, path);
  }

  std::unique_ptr<ox::App> app = nullptr;
  std::filesystem::path directory = {};
};

TEST_F(AssetStagingTest, StagedParentPublishedOnDemandHoldsItsStagedChildren) {
  const auto material = asset_man().create_asset(ox::AssetType::Material, directory / "spark_material");
  const auto system = particle_system_with(material);

  on_worker([&] { ASSERT_TRUE(asset_man().load_asset(system)); });
  ASSERT_TRUE(asset_man().is_loading(material));
  ASSERT_TRUE(asset_man().is_loading(system));

  // the main thread publishes the staged system before the commit, its material has to come with it
  ASSERT_TRUE(asset_man().load_asset(system));
  EXPECT_TRUE(asset_man().is_loaded(material));
  EXPECT_TRUE(asset_man().get_material(material));
  EXPECT_EQ(ref_count(material), 1_u64);

  asset_man().commit();
  EXPECT_EQ(ref_count(material), 1_u64);

  // the worker's load and the main thread's both took one
  asset_man().unload_asset(system);
  asset_man().unload_asset(system);
  asset_man().commit();
  EXPECT_FALSE(asset_man().is_loaded(system));
  EXPECT_FALSE(asset_man().is_loaded(material));
}

TEST_F(AssetStagingTest, CommitPublishesChildrenOfAStagedParent) {
  const auto material = asset_man().create_asset(ox::AssetType::Material, directory / "spark_material");
  const auto system = particle_system_with(material);

  on_worker([&] { ASSERT_TRUE(asset_man().load_asset(system)); });
  asset_man().commit();

  EXPECT_TRUE(asset_man().is_loaded(system));
  EXPECT_TRUE(asset_man().is_loaded(material));
  EXPECT_EQ(ref_count(material), 1_u64);

  asset_man().unload_asset(system);
  asset_man().commit();
  EXPECT_FALSE(asset_man().is_loaded(material));
}

TEST_F(AssetStagingTest, HandleAcquiredOnAWorkerResolvesAtTheCommit) {
  const auto material = asset_man().create_asset(ox::AssetType::Material, directory / "stone");

  auto handle = ox::AssetHandle<ox::Material>();
  on_worker([&] { handle = ox::AssetHandle<ox::Material>::acquire(material); });
  EXPECT_FALSE(handle.is_loaded());
  EXPECT_EQ(ref_count(material), 1_u64);

  asset_man().commit();
  EXPECT_TRUE(handle.is_loaded());
  EXPECT_TRUE(handle.get());

  // a copy of a ref on a staged asset counts too
  on_worker([&] {
    auto staged = ox::AssetHandle<ox::Material>::acquire(asset_man().create_asset(ox::AssetType::Material));
    auto copy = staged;
    EXPECT_EQ(ref_count(copy.uuid()), 2_u64);
  });

  handle = {};
  asset_man().commit();
  EXPECT_FALSE(asset_man().is_loaded(material));
}

TEST_F(AssetStagingTest, DeletingAStagedAssetDoesNotReviveItStaged) {
  const auto path = directory / "stone";
  const auto material = asset_man().create_asset(ox::AssetType::Material, path);

  on_worker([&] { ASSERT_TRUE(asset_man().load_asset(material)); });
  ASSERT_TRUE(asset_man().is_loading(material));

  asset_man().delete_asset(material);
  asset_man().commit();

  ASSERT_TRUE(asset_man().register_asset(material, ox::AssetType::Material, path));
  EXPECT_FALSE(asset_man().is_loading(material));
  EXPECT_EQ(ref_count(material), 0_u64);

  ASSERT_TRUE(asset_man().load_asset(material));
  EXPECT_TRUE(asset_man().get_material(material));
  asset_man().unload_asset(material);
}
