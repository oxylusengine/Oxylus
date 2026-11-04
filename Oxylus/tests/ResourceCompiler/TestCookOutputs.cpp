#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

#include "Asset/AssetFile.hpp"
#include "AssetImport.hpp"
#include "ResourceCompiler.hpp"

using namespace ox;

static auto write_text(const std::filesystem::path& path, std::string_view text) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// what the one UI pack in `output` holds, empty when there is none
static auto cooked_text(const std::filesystem::path& output) -> std::string {
  for (const auto& entry : std::filesystem::directory_iterator(output)) {
    if (entry.path().extension() != ".oxpack") {
      continue;
    }

    const auto file = AssetFile::unpack(entry.path());
    if (!file.has_value()) {
      continue;
    }

    for (const auto& packed : file->entries) {
      if (const auto* ui = std::get_if<UIData>(&packed.data)) {
        return ui->text;
      }
    }
  }

  return {};
}

// the sidecar's hash is shared by every output, the editor's cache and each build of a game, so one of them catching
// up with a changed source must not leave another on the pack it cooked before
TEST(CookOutputs, EachOutputRecooksAChangedSource) {
  const auto root = std::filesystem::temp_directory_path() / "ox_test_cook_outputs";
  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
  const auto assets = root / "Assets";
  write_text(assets / "UI" / "hud.rml", "<rml>first</rml>");

  auto session = rc::Session::create();
  ASSERT_TRUE(session.has_value());
  ASSERT_TRUE(rc::cook_assets(*session, assets, root / "debug"));
  ASSERT_TRUE(rc::cook_assets(*session, assets, root / "release"));
  EXPECT_EQ(cooked_text(root / "debug"), "<rml>first</rml>");

  write_text(assets / "UI" / "hud.rml", "<rml>second</rml>");
  ASSERT_TRUE(rc::cook_assets(*session, assets, root / "release"));
  ASSERT_TRUE(rc::cook_assets(*session, assets, root / "debug"));
  EXPECT_EQ(cooked_text(root / "release"), "<rml>second</rml>");
  EXPECT_EQ(cooked_text(root / "debug"), "<rml>second</rml>");
  EXPECT_TRUE(session->get_errors().empty());

  session->destroy();
  std::filesystem::remove_all(root, error);
}

TEST(CookOutputs, ForcedCookRebuildsEveryPack) {
  const auto root = std::filesystem::temp_directory_path() / "ox_test_cook_forced";
  auto error = std::error_code{};
  std::filesystem::remove_all(root, error);
  const auto assets = root / "Assets";
  write_text(assets / "UI" / "hud.rml", "<rml>hud</rml>");

  auto session = rc::Session::create();
  ASSERT_TRUE(session.has_value());
  ASSERT_TRUE(rc::cook_assets(*session, assets, root / "out"));
  std::ignore = session->take_diagnostics();

  ASSERT_TRUE(rc::cook_assets(*session, assets, root / "out", true));
  const auto diagnostics = session->take_diagnostics();
  EXPECT_TRUE(diagnostics.errors.empty());
  EXPECT_TRUE(std::ranges::any_of(diagnostics.messages, [](const std::string& message) {
    return message.starts_with("Removed 1 packs");
  }));
  EXPECT_EQ(cooked_text(root / "out"), "<rml>hud</rml>");

  session->destroy();
  std::filesystem::remove_all(root, error);
}
