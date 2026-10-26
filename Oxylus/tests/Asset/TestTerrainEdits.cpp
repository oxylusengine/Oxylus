#include <gtest/gtest.h>

#include "Asset/TerrainEdits.hpp"
#include "Utils/Log.hpp"

using namespace ox;

// a header and two maps go out in three writes and come back in three reads, which is what any file format that isn't
// built in memory first looks like
TEST(TerrainEditsTest, RoundTripsThroughAFile) {
  loguru::g_stderr_verbosity = loguru::Verbosity_OFF;

  const auto path = std::filesystem::temp_directory_path() / "ox_terrain_edits_test.oxterrain";

  auto edits = TerrainEdits{.resolution = {4, 2}};
  // 4 bytes a texel
  edits.height.resize(4 * 2 * 4);
  edits.splat.resize(4 * 2 * 4);
  for (auto i = 0_sz; i < edits.height.size(); i++) {
    edits.height[i] = static_cast<u8>(i);
    edits.splat[i] = static_cast<u8>(0xFF - i);
  }

  ASSERT_TRUE(edits.write(path));

  const auto read = TerrainEdits::read(path);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->resolution, edits.resolution);
  EXPECT_EQ(read->height, edits.height);
  EXPECT_EQ(read->splat, edits.splat);

  std::filesystem::remove(path);
}
