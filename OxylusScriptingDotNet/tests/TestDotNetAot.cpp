#include <gtest/gtest.h>

#include "DotNetTestHelpers.hpp"

class DotNetAotTest : public ::testing::Test {
protected:
  void SetUp() override { captured_logs.clear(); }

  void TearDown() override { scripting.unload(); }

  ox::DotNetScripting scripting = {};
  ox::DotNetLoadInfo info = aot_smoke_game();
};

TEST_F(DotNetAotTest, LoadsSmokeGameAndRoundTripsItsLog) {
  const auto result = scripting.load(info, capturing_native_api());
  ASSERT_TRUE(result.has_value()) << result.error();

  EXPECT_TRUE(scripting.is_loaded());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "hello from SmokeGame"), 1);
}

TEST_F(DotNetAotTest, RejectsAMismatchedAbiVersion) {
  info.abi_version = OX_DOTNET_ABI_VERSION + 1;

  const auto result = scripting.load(info, capturing_native_api());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("ABI"), std::string::npos) << result.error();
  EXPECT_FALSE(scripting.is_loaded());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "hello from SmokeGame"), 0);
}

// the library stays mapped, a NativeAOT runtime can't be unloaded, so this is init running again on the same image
TEST_F(DotNetAotTest, ShutsDownAndInitializesAgain) {
  ASSERT_TRUE(scripting.load(info, capturing_native_api()).has_value());
  scripting.unload();
  EXPECT_FALSE(scripting.is_loaded());

  ASSERT_TRUE(scripting.load(info, capturing_native_api()).has_value());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "hello from SmokeGame"), 2);
}

TEST_F(DotNetAotTest, FailsOnAMissingLibrary) {
  info.path = binary_dir() / "aot" / "Missing.dll";

  const auto result = scripting.load(info, capturing_native_api());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("Couldn't load"), std::string::npos) << result.error();
  EXPECT_FALSE(scripting.is_loaded());
}
