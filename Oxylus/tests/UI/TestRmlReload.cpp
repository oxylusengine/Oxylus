#include <RmlUi/Core.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

#include "UI/RmlUI.hpp"
#include "Utils/Log.hpp"

using namespace ox;

class NullRenderInterface final : public Rml::RenderInterface {
public:
  auto CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) -> Rml::CompiledGeometryHandle override {
    return 1;
  }

  auto RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) -> void override {}
  auto ReleaseGeometry(Rml::CompiledGeometryHandle) -> void override {}
  auto LoadTexture(Rml::Vector2i&, const Rml::String&) -> Rml::TextureHandle override { return 0; }
  auto GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) -> Rml::TextureHandle override { return 1; }
  auto ReleaseTexture(Rml::TextureHandle) -> void override {}
  auto EnableScissorRegion(bool) -> void override {}
  auto SetScissorRegion(Rml::Rectanglei) -> void override {}
};

class RmlReloadTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
    Rml::Initialise();
  }

  static void TearDownTestSuite() { Rml::Shutdown(); }

  void SetUp() override {
    directory = std::filesystem::temp_directory_path() / "ox_rml_reload_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    write("style.rcss", "#box { display: block; width: 10px; height: 10px; }");
    write(
      "hud.rml",
      R"(<rml><head><link type="text/rcss" href="style.rcss"/></head><body><div id="box"/></body></rml>)"
    );

    context = Rml::CreateContext("rml_reload_test", {200, 200}, &render);
    document = context->LoadDocument(path("hud.rml"));
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }

  void TearDown() override {
    Rml::RemoveContext("rml_reload_test");
    std::filesystem::remove_all(directory);
  }

  auto path(std::string_view name) const -> std::string { return (directory / name).generic_string(); }

  auto write(std::string_view name, std::string_view contents) const -> void {
    auto file = std::ofstream(directory / name, std::ios::binary | std::ios::trunc);
    file << contents;
  }

  auto reload(std::string_view name) -> void {
    const auto paths = std::vector{path(name)};
    reload_rml_documents(context, paths);
    context->Update();
  }

  inline static NullRenderInterface render = {};
  std::filesystem::path directory = {};
  Rml::Context* context = nullptr;
  Rml::ElementDocument* document = nullptr;
};

TEST_F(RmlReloadTest, AnEditedStyleSheetRestylesTheSameElements) {
  auto* box = document->GetElementById("box");
  ASSERT_NE(box, nullptr);
  EXPECT_FLOAT_EQ(box->GetBox().GetSize().x, 10.0f);

  write("style.rcss", "#box { display: block; width: 20px; height: 10px; }");
  reload("style.rcss");

  ASSERT_EQ(context->GetNumDocuments(), 1);
  EXPECT_EQ(context->GetDocument(0), document);
  EXPECT_EQ(document->GetElementById("box"), box);
  EXPECT_FLOAT_EQ(box->GetBox().GetSize().x, 20.0f);
}

TEST_F(RmlReloadTest, AnEditedDocumentTakesItsOwnPlace) {
  write(
    "hud.rml",
    R"(<rml><head><link type="text/rcss" href="style.rcss"/></head><body><div id="box"/><div id="added"/></body></rml>)"
  );
  reload("hud.rml");

  ASSERT_EQ(context->GetNumDocuments(), 1);
  auto* replacement = context->GetDocument(0);
  EXPECT_NE(replacement, document);
  EXPECT_NE(replacement->GetElementById("added"), nullptr);
  EXPECT_TRUE(replacement->IsVisible());
}

TEST_F(RmlReloadTest, AHiddenDocumentStaysHidden) {
  document->Hide();
  context->Update();
  reload("hud.rml");

  ASSERT_EQ(context->GetNumDocuments(), 1);
  EXPECT_NE(context->GetDocument(0), document);
  EXPECT_FALSE(context->GetDocument(0)->IsVisible());
}

TEST_F(RmlReloadTest, AnUnrelatedFileLeavesTheDocumentAlone) {
  write("other.rml", "<rml><body/></rml>");
  reload("other.rml");

  ASSERT_EQ(context->GetNumDocuments(), 1);
  EXPECT_EQ(context->GetDocument(0), document);
}
