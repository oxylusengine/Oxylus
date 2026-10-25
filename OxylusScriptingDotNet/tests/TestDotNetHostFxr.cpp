#include <gtest/gtest.h>

#include "DotNetTestHelpers.hpp"

// CoreCLR starts once per process, so everything here shares one runtime and only the scripts come and go
class DotNetHostFxrTest : public ::testing::Test {
protected:
  void SetUp() override { captured_logs.clear(); }

  void TearDown() override { scripting.unload(); }

  ox::DotNetScripting scripting = {};
  ox::DotNetLoadInfo info = hostfxr_smoke_game();
};

TEST_F(DotNetHostFxrTest, LoadsSmokeGameAndRoundTripsItsLog) {
  const auto result = scripting.load(info, capturing_native_api());
  ASSERT_TRUE(result.has_value()) << result.error();

  EXPECT_TRUE(scripting.is_loaded());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "hello from SmokeGame"), 1);
}

TEST_F(DotNetHostFxrTest, RejectsAMismatchedAbiVersion) {
  info.abi_version = OX_DOTNET_ABI_VERSION + 1;

  const auto result = scripting.load(info, capturing_native_api());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("ABI"), std::string::npos) << result.error();
  EXPECT_FALSE(scripting.is_loaded());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "hello from SmokeGame"), 0);
}

TEST_F(DotNetHostFxrTest, UnloadsTheScriptsAndLoadsThemAgain) {
  ASSERT_TRUE(scripting.load(info, capturing_native_api()).has_value());
  scripting.unload();
  EXPECT_FALSE(scripting.is_loaded());

  // the collectible context warns when something still holds the scripts after unloading
  EXPECT_EQ(count_logs(ox::OxLogLevel::Warn), 0);

  ASSERT_TRUE(scripting.load(info, capturing_native_api()).has_value());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "hello from SmokeGame"), 2);
}

TEST_F(DotNetHostFxrTest, FailsOnAMissingAssembly) {
  info.path = binary_dir() / "managed" / "Missing.dll";

  const auto result = scripting.load(info, capturing_native_api());
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(scripting.is_loaded());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 1);
}
