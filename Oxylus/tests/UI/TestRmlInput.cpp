#include <RmlUi/Core.h>
#include <gtest/gtest.h>

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

class RmlInputTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    loguru::g_stderr_verbosity = loguru::Verbosity_OFF;
    Rml::Initialise();
  }

  static void TearDownTestSuite() { Rml::Shutdown(); }

  void SetUp() override { context = Rml::CreateContext("rml_input_test", {800, 600}, &render); }

  void TearDown() override { Rml::RemoveContext("rml_input_test"); }

  auto load(const Rml::String& rml) -> Rml::ElementDocument* {
    auto* document = context->LoadDocumentFromMemory(rml);
    document->Show();
    context->Update();
    return document;
  }

  inline static NullRenderInterface render = {};
  Rml::Context* context = nullptr;
};

TEST_F(RmlInputTest, OnlyAFocusedTextFieldWantsTheKeyboard) {
  auto* document = load(R"rml(<rml><head></head><body>
    <input id="text" type="text"/>
    <input id="password" type="password"/>
    <input id="untyped"/>
    <textarea id="area"></textarea>
    <input id="check" type="checkbox"/>
    <input id="range" type="range"/>
    <button id="button">ok</button>
  </body></rml>)rml");

  EXPECT_FALSE(rml_wants_keyboard(nullptr));
  EXPECT_FALSE(rml_wants_keyboard(context));

  for (const auto* id : {"text", "password", "untyped", "area"}) {
    ASSERT_TRUE(document->GetElementById(id)->Focus()) << id;
    EXPECT_TRUE(rml_wants_keyboard(context)) << id;
  }

  for (const auto* id : {"check", "range", "button"}) {
    ASSERT_TRUE(document->GetElementById(id)->Focus()) << id;
    EXPECT_FALSE(rml_wants_keyboard(context)) << id;
  }

  document->GetElementById("text")->Focus();
  document->GetElementById("text")->Blur();
  EXPECT_FALSE(rml_wants_keyboard(context));
}

// what keeps a full-screen HUD from eating every click: the capture follows RmlUi's own idea of interaction
TEST_F(RmlInputTest, APointerEventsNoneBodyLetsTheGameHaveTheMouse) {
  load(R"rml(<rml><head></head>
    <body style="width: 100%; height: 100%; pointer-events: none;">
      <div style="display: block; width: 100px; height: 100px; pointer-events: auto;"></div>
    </body></rml>)rml");

  context->ProcessMouseMove(50, 50, 0);
  EXPECT_TRUE(context->IsMouseInteracting());

  context->ProcessMouseMove(500, 500, 0);
  EXPECT_FALSE(context->IsMouseInteracting());
}

TEST_F(RmlInputTest, AFullScreenBodyTakesTheMouseByDefault) {
  load(R"rml(<rml><head></head><body style="width: 100%; height: 100%;"></body></rml>)rml");

  context->ProcessMouseMove(500, 500, 0);
  EXPECT_TRUE(context->IsMouseInteracting());
}
