#include <array>
#include <gtest/gtest.h>
#include <string_view>

#include "OS/File.hpp"
#include "Utils/Log.hpp"

using namespace ox;

class FileTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;

    directory = std::filesystem::temp_directory_path() / "ox_file_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
  }

  void TearDown() override { std::filesystem::remove_all(directory); }

  std::filesystem::path directory = {};
};

TEST_F(FileTest, WritesFollowOneAnother) {
  const auto path = directory / "appended.txt";
  {
    auto file = File(path, FileAccess::Write);
    ASSERT_TRUE(static_cast<bool>(file));
    EXPECT_EQ(file.write(std::string_view("head")), 4);
    EXPECT_EQ(file.write(std::string_view("-body")), 5);
    EXPECT_EQ(file.write(std::string_view("-tail")), 5);
  }

  EXPECT_EQ(File::to_string(path), "head-body-tail");
}

TEST_F(FileTest, ReadsFollowOneAnother) {
  const auto path = directory / "sequential.txt";
  {
    auto file = File(path, FileAccess::Write);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(std::string_view("abcdefgh"));
  }

  auto file = File(path, FileAccess::Read);
  ASSERT_TRUE(static_cast<bool>(file));

  auto first = std::array<c8, 3>{};
  auto second = std::array<c8, 3>{};
  ASSERT_EQ(file.read(first.data(), first.size()), 3);
  ASSERT_EQ(file.read(second.data(), second.size()), 3);
  EXPECT_EQ(std::string_view(first.data(), first.size()), "abc");
  EXPECT_EQ(std::string_view(second.data(), second.size()), "def");

  // only two bytes left, a read asking for more stops at the end rather than spinning
  auto rest = std::array<c8, 8>{};
  EXPECT_EQ(file.read(rest.data(), rest.size()), 2);
  EXPECT_EQ(std::string_view(rest.data(), 2), "gh");
}

TEST_F(FileTest, SeekMovesWhereTheNextReadStarts) {
  const auto path = directory / "seek.txt";
  {
    auto file = File(path, FileAccess::Write);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(std::string_view("0123456789"));
  }

  auto file = File(path, FileAccess::Read);
  ASSERT_TRUE(static_cast<bool>(file));

  auto bytes = std::array<c8, 3>{};
  file.seek(6);
  ASSERT_EQ(file.read(bytes.data(), bytes.size()), 3);
  EXPECT_EQ(std::string_view(bytes.data(), bytes.size()), "678");

  file.seek(1);
  ASSERT_EQ(file.read(bytes.data(), bytes.size()), 3);
  EXPECT_EQ(std::string_view(bytes.data(), bytes.size()), "123");
}
