#include <array>
#include <cstdio>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

#include "Asset/AssetFile.hpp"
#include "Asset/AssetManager.hpp"
#include "Core/VFS.hpp"
#include "OS/File.hpp"
#include "UI/RmlFileInterface.hpp"
#include "Utils/Log.hpp"

using namespace ox;

class RmlFileInterfaceTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;

    directory = std::filesystem::temp_directory_path() / "ox_rml_file_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory / "UI");
    {
      auto file = File(directory / "UI" / "hud.rml", FileAccess::Write);
      file.write(std::string_view("<rml>hud</rml>"));
    }

    vfs.mount_dir(VFS::ASSETS_DIR, directory);
    files.vfs = &vfs;
  }

  void TearDown() override { std::filesystem::remove_all(directory); }

  std::filesystem::path directory = {};
  VFS vfs = {};
  RmlFileInterface files = {};
};

TEST_F(RmlFileInterfaceTest, ResolvesAVirtualPathThroughItsMount) {
  EXPECT_EQ(resolve_rml_path(&vfs, "assets_dir/UI/hud.rml"), (directory / "UI" / "hud.rml").lexically_normal());
  // what RmlUi joins a linked stylesheet against a virtual document to
  EXPECT_EQ(resolve_rml_path(&vfs, "assets_dir/UI/style.rcss"), (directory / "UI" / "style.rcss").lexically_normal());
}

TEST_F(RmlFileInterfaceTest, LeavesAnythingElseAsItIs) {
  const auto absolute = (directory / "UI" / "hud.rml").string();
  EXPECT_EQ(resolve_rml_path(&vfs, absolute), std::filesystem::path(absolute));
  EXPECT_EQ(resolve_rml_path(&vfs, "UI/hud.rml"), std::filesystem::path("UI/hud.rml"));
  EXPECT_EQ(resolve_rml_path(nullptr, "assets_dir/UI/hud.rml"), std::filesystem::path("assets_dir/UI/hud.rml"));
}

TEST_F(RmlFileInterfaceTest, ReadsAndSeeksAnOpenFile) {
  const auto handle = files.Open("assets_dir/UI/hud.rml");
  ASSERT_NE(handle, 0);
  EXPECT_EQ(files.Length(handle), 14);

  auto bytes = std::array<char, 5>{};
  EXPECT_EQ(files.Read(bytes.data(), bytes.size(), handle), 5);
  EXPECT_EQ(std::string_view(bytes.data(), bytes.size()), "<rml>");
  EXPECT_EQ(files.Tell(handle), 5);

  EXPECT_TRUE(files.Seek(handle, -6, SEEK_END));
  EXPECT_EQ(files.Read(bytes.data(), bytes.size(), handle), 5);
  EXPECT_EQ(std::string_view(bytes.data(), bytes.size()), "</rml");

  // only one byte left, and nothing past the end
  EXPECT_EQ(files.Read(bytes.data(), bytes.size(), handle), 1);
  EXPECT_FALSE(files.Seek(handle, 1, SEEK_CUR));
  EXPECT_FALSE(files.Seek(handle, -1, SEEK_SET));

  files.Close(handle);
}

TEST_F(RmlFileInterfaceTest, LoadsAWholeFile) {
  auto contents = Rml::String{};
  ASSERT_TRUE(files.LoadFile("assets_dir/UI/hud.rml", contents));
  EXPECT_EQ(contents, "<rml>hud</rml>");
}

// what a shipped game sees: the source stripped, a pack in the cook registered by the source's path
TEST_F(RmlFileInterfaceTest, ReadsTheCookedPackOnceTheSourceIsGone) {
  auto asset_man = AssetManager{};
  ASSERT_TRUE(asset_man.init().has_value());
  files.asset_man = &asset_man;

  const auto pack_path = directory / "style.oxpack";
  auto pack = AssetFile{};
  pack.add_entry(UIData{.name = "style.rcss", .text = "body { color: red; }"});
  ASSERT_TRUE(pack.pack(pack_path));
  // tests run without an app, so the registry keys a path the same way on both sides
  ASSERT_TRUE(asset_man.register_asset(UUID::generate_random(), AssetType::UI, pack_path, "assets_dir/UI/style.rcss"));

  auto contents = Rml::String{};
  ASSERT_TRUE(files.LoadFile("assets_dir/UI/style.rcss", contents));
  EXPECT_EQ(contents, "body { color: red; }");

  const auto handle = files.Open("assets_dir/UI/style.rcss");
  ASSERT_NE(handle, 0);
  EXPECT_EQ(files.Length(handle), 20);
  files.Close(handle);

  EXPECT_TRUE(asset_man.deinit().has_value());
}

// while the source is there it is the newer of the two, the one being edited
TEST_F(RmlFileInterfaceTest, PrefersTheSourceToItsPack) {
  auto asset_man = AssetManager{};
  ASSERT_TRUE(asset_man.init().has_value());
  files.asset_man = &asset_man;

  const auto pack_path = directory / "hud.oxpack";
  auto pack = AssetFile{};
  pack.add_entry(UIData{.name = "hud.rml", .text = "<rml>stale</rml>"});
  ASSERT_TRUE(pack.pack(pack_path));
  ASSERT_TRUE(asset_man.register_asset(UUID::generate_random(), AssetType::UI, pack_path, "assets_dir/UI/hud.rml"));

  auto contents = Rml::String{};
  ASSERT_TRUE(files.LoadFile("assets_dir/UI/hud.rml", contents));
  EXPECT_EQ(contents, "<rml>hud</rml>");

  EXPECT_TRUE(asset_man.deinit().has_value());
}

// a font's bytes, zeros and all, from its pack once the source is gone: how `LoadFontFace` and ImGui both get one
TEST_F(RmlFileInterfaceTest, ReadsACookedFontByteForByte) {
  auto asset_man = AssetManager{};
  ASSERT_TRUE(asset_man.init().has_value());

  const auto font_bytes = std::vector<u8>{0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0xFF, 0x00};
  const auto pack_path = directory / "font.oxpack";
  auto pack = AssetFile{};
  pack.add_entry(FontData{.name = "game.ttf", .bytes = font_bytes});
  ASSERT_TRUE(pack.pack(pack_path));
  ASSERT_TRUE(
    asset_man.register_asset(UUID::generate_random(), AssetType::Font, pack_path, "assets_dir/Fonts/game.ttf")
  );

  const auto contents = read_ui_file(&vfs, &asset_man, "assets_dir/Fonts/game.ttf");
  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(std::vector<u8>(contents->begin(), contents->end()), font_bytes);

  // only an asset of a type the UI reads is taken, anything else under that path is not a font
  EXPECT_TRUE(find_rml_pack(asset_man, &vfs, "assets_dir/Fonts/game.ttf", {AssetType::Texture}).empty());

  EXPECT_TRUE(asset_man.deinit().has_value());
}

TEST_F(RmlFileInterfaceTest, FailsOnAMissingFile) {
  EXPECT_EQ(files.Open("assets_dir/UI/missing.rml"), 0);

  auto contents = Rml::String{};
  EXPECT_FALSE(files.LoadFile("assets_dir/UI/missing.rml", contents));
}
