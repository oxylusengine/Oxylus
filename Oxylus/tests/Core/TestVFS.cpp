#include <gtest/gtest.h>

#include "Core/VFS.hpp"
#include "Utils/Log.hpp"

class VFSTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;

    root = std::filesystem::temp_directory_path() / "ox_vfs_test";
    vfs.mount_dir(ox::VFS::APP_DIR, root / "App");
    vfs.mount_dir(ox::VFS::ASSETS_DIR, root / "Project" / "Assets");
    vfs.mount_dir(ox::VFS::COOKED_DIR, root / "Project" / "Assets" / ".cooked");
  }

  ox::VFS vfs = {};
  std::filesystem::path root = {};
};

TEST_F(VFSTest, ResolvesVirtualPathsToTheirMount) {
  EXPECT_EQ(vfs.to_physical("assets_dir/Audio/engine.wav"), root / "Project" / "Assets" / "Audio" / "engine.wav");
  EXPECT_EQ(vfs.to_physical("app_dir/Fonts/a.ttf"), root / "App" / "Fonts" / "a.ttf");
}

TEST_F(VFSTest, MapsPhysicalPathsUnderAMountToVirtual) {
  const auto physical = root / "Project" / "Assets" / "Audio" / "engine.wav";
  EXPECT_EQ(vfs.to_virtual(physical), std::filesystem::path("assets_dir/Audio/engine.wav"));
  EXPECT_EQ(vfs.to_physical(vfs.to_virtual(physical)), physical);
}

TEST_F(VFSTest, LeavesVirtualPathsUnchanged) {
  EXPECT_EQ(
    vfs.to_virtual("assets_dir/Audio/../Audio/engine.wav"),
    std::filesystem::path("assets_dir/Audio/engine.wav")
  );
}

TEST_F(VFSTest, PassesThroughPathsOutsideEveryMount) {
  const auto outside = root / "Elsewhere" / "x.wav";
  EXPECT_EQ(vfs.to_virtual(outside), outside);
  EXPECT_EQ(vfs.to_physical(outside), outside);
}

TEST_F(VFSTest, DoesNotMatchASiblingWithTheSamePrefix) {
  const auto sibling = root / "Project" / "Assets2" / "x.wav";
  EXPECT_EQ(vfs.to_virtual(sibling), sibling);
}

TEST_F(VFSTest, PrefersTheMostSpecificMount) {
  const auto cooked = root / "Project" / "Assets" / ".cooked" / "a.oxpack";
  vfs.unmount_dir(ox::VFS::ASSETS_DIR);
  vfs.mount_dir("outer", root / "Project");
  EXPECT_EQ(vfs.to_virtual(cooked), std::filesystem::path("cooked_dir/a.oxpack"));
}

TEST_F(VFSTest, PrefersAssetsDirWhenAppDirSharesItsFolder) {
  vfs.mount_dir(ox::VFS::APP_DIR, root / "Project" / "Assets");
  EXPECT_EQ(vfs.to_virtual(root / "Project" / "Assets" / "x.wav"), std::filesystem::path("assets_dir/x.wav"));
}

TEST_F(VFSTest, MapsAMountRootWithoutATrailingSeparator) {
  EXPECT_EQ(vfs.to_virtual(root / "App"), std::filesystem::path("app_dir"));
}

TEST_F(VFSTest, RemountingReplacesThePhysicalDir) {
  vfs.mount_dir(ox::VFS::ASSETS_DIR, root / "Other");
  EXPECT_EQ(vfs.to_physical("assets_dir/x.wav"), root / "Other" / "x.wav");
}

TEST_F(VFSTest, AnUnmountedVirtualDirResolvesToNothing) {
  vfs.unmount_dir(ox::VFS::ASSETS_DIR);
  EXPECT_TRUE(vfs.to_physical("assets_dir/x.wav").empty());
}
