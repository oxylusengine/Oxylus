#include <gtest/gtest.h>

#include "Asset/AssetManager.hpp"
#include "Asset/AssetManifest.hpp"
#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "OS/File.hpp"
#include "Utils/Log.hpp"

// laid out like a shipped game: sources under Assets, the export under Assets/.cooked
class AssetManifestTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    static char arg0[] = "TestAssetManifest";
    static char* argv[] = {arg0, nullptr};
    app = std::make_unique<ox::App>(1, argv);
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
  }

  static void TearDownTestSuite() { app.reset(); }

  void SetUp() override {
    root = std::filesystem::temp_directory_path() / "ox_asset_manifest_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Assets" / ox::VFS::COOKED_SUBDIR);

    auto& vfs = ox::App::get_vfs();
    vfs.mount_dir(ox::VFS::ASSETS_DIR, root / "Assets");
    vfs.mount_dir(ox::VFS::COOKED_DIR, root / "Assets" / ox::VFS::COOKED_SUBDIR);
  }

  void TearDown() override { std::filesystem::remove_all(root); }

  auto manifest_path() const -> std::filesystem::path {
    return root / "Assets" / ox::VFS::COOKED_SUBDIR / ox::AssetManifest::FILE_NAME;
  }

  static auto red_material() -> ox::Material {
    auto material = ox::Material{};
    material.albedo_color = {1.0f, 0.0f, 0.0f, 1.0f};
    material.roughness_factor = 0.25f;
    material.alpha_mode = ox::AlphaMode::Blend;
    material.albedo_texture = texture_uuid;
    return material;
  }

  static inline std::unique_ptr<ox::App> app = nullptr;
  static inline const auto texture_uuid = ox::UUID::generate_random();
  std::filesystem::path root = {};
};

TEST_F(AssetManifestTest, RoundTripsEntriesAndMaterials) {
  const auto uuid = ox::UUID::generate_random();
  auto manifest = ox::AssetManifest{};
  manifest.assets.push_back(
    {.uuid = ox::PackedUUID::pack(uuid),
     .type = ox::AssetType::Model,
     .path = "cooked_dir/car.oxpack",
     .source_path = "assets_dir/Models/car.glb"}
  );
  manifest.materials.push_back(ox::AssetManifest::MaterialEntry::pack(uuid, red_material()));
  ASSERT_TRUE(manifest.write(manifest_path()));

  const auto read = ox::AssetManifest::read(manifest_path());
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(read->assets.size(), 1);
  EXPECT_EQ(read->assets[0].uuid.unpack(), uuid);
  EXPECT_EQ(read->assets[0].type, ox::AssetType::Model);
  EXPECT_EQ(read->assets[0].path, "cooked_dir/car.oxpack");
  EXPECT_EQ(read->assets[0].source_path, "assets_dir/Models/car.glb");

  ASSERT_EQ(read->materials.size(), 1);
  const auto material = read->materials[0].unpack();
  EXPECT_EQ(material.albedo_color, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
  EXPECT_FLOAT_EQ(material.roughness_factor, 0.25f);
  EXPECT_EQ(material.alpha_mode, ox::AlphaMode::Blend);
  EXPECT_EQ(material.albedo_texture, texture_uuid);
  EXPECT_FALSE(material.normal_texture);
}

TEST_F(AssetManifestTest, RejectsAFileThatIsNotAManifest) {
  auto file = ox::File(manifest_path(), ox::FileAccess::Write);
  file.write(std::string_view("definitely not a manifest"));
  file.close();

  EXPECT_FALSE(ox::AssetManifest::read(manifest_path()).has_value());
}

TEST_F(AssetManifestTest, InitRegistersEverythingTheManifestLists) {
  const auto model_uuid = ox::UUID::generate_random();
  const auto material_uuid = ox::UUID::generate_random();
  auto manifest = ox::AssetManifest{};
  manifest.assets.push_back(
    {.uuid = ox::PackedUUID::pack(model_uuid),
     .type = ox::AssetType::Model,
     .path = "cooked_dir/car.oxpack",
     .source_path = "assets_dir/Models/car.glb"}
  );
  manifest.assets.push_back(
    {.uuid = ox::PackedUUID::pack(material_uuid),
     .type = ox::AssetType::Material,
     .path = "assets_dir/Models/car.glb",
     .source_path = ""}
  );
  manifest.materials.push_back(ox::AssetManifest::MaterialEntry::pack(material_uuid, red_material()));
  ASSERT_TRUE(manifest.write(manifest_path()));

  auto asset_man = ox::AssetManager{};
  ASSERT_TRUE(asset_man.init().has_value());

  EXPECT_EQ(asset_man.find_asset("assets_dir/Models/car.glb"), model_uuid);
  EXPECT_EQ(asset_man.find_asset(root / "Assets" / "Models" / "car.glb"), model_uuid);
  {
    auto model = asset_man.get_asset(model_uuid);
    ASSERT_TRUE(static_cast<bool>(model));
    EXPECT_EQ(ox::App::get_vfs().to_physical(model->path), root / "Assets" / ox::VFS::COOKED_SUBDIR / "car.oxpack");
  }

  ASSERT_TRUE(asset_man.load_asset(material_uuid));
  {
    auto material = asset_man.get_material(material_uuid);
    ASSERT_TRUE(static_cast<bool>(material));
    EXPECT_EQ(material->albedo_color, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_EQ(material->albedo_texture, texture_uuid);
  }
  asset_man.unload_asset(material_uuid);

  EXPECT_TRUE(asset_man.deinit().has_value());
}

TEST_F(AssetManifestTest, InitWithoutAManifestRegistersNothingExtra) {
  auto asset_man = ox::AssetManager{};
  ASSERT_TRUE(asset_man.init().has_value());
  // the null material is the only asset every init creates
  EXPECT_EQ(asset_man.get_registry_snapshot().size(), 1);
  EXPECT_TRUE(asset_man.deinit().has_value());
}
