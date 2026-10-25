#pragma once

// RmlUi documents, listeners and data models from C#, in a real RmlUi context with no GPU behind it: a stub render
// interface and the engine's VFS file interface. Each test binary defines smoke_game() first

#include <RmlUi/Core.h>
#include <filesystem>
#include <flecs.h>
#include <fstream>
#include <gtest/gtest.h>

#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "DotNetTestHelpers.hpp"
#include "Scene/Components.hpp"
#include "UI/RmlFileInterface.hpp"

// SmokeGame.UiProbe, as flecs lays out its members
struct UiProbe {
  i32 phase = 0;
  i32 score = 0;
  i32 listener_hits = 0;
};

// lays out and updates documents, draws nothing
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

class DotNetUiTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    static char arg0[] = "TestDotNetUi";
    static char* argv[] = {arg0, nullptr};
    loguru::g_stderr_verbosity = loguru::Verbosity_WARNING;
    app = std::make_unique<ox::App>(1, argv);
    app->init();

    root = std::filesystem::temp_directory_path() / "ox_dotnet_ui_test";
    std::filesystem::create_directories(root / "UI");
    auto document = std::ofstream(root / "UI" / "probe.rml", std::ios::binary);
    document << R"rml(<rml>
<head></head>
<body data-model="probe">
  <h1 id="title">Title</h1>
  <span id="score">{{score}}</span>
  <span id="label">{{label}}</span>
  <button id="button" data-event-click="add(5)">add</button>
</body>
</rml>
)rml";
    document.close();
    ox::App::get_vfs().mount_dir(ox::VFS::ASSETS_DIR, root);

    files.vfs = &ox::App::get_vfs();
    Rml::SetFileInterface(&files);
    Rml::SetRenderInterface(&render);
    Rml::Initialise();
  }

  static void TearDownTestSuite() {
    Rml::Shutdown();
    app->stop();
    app.reset();
  }

  void SetUp() override {
    captured_logs.clear();
    context = Rml::CreateContext("dotnet_ui_test", {800, 600}, &render);
    ASSERT_NE(context, nullptr);

    world.import<ox::CoreComponentsModule>();
    const auto loaded = scripting.load(smoke_game(), capturing_native_api());
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    ASSERT_TRUE(scripting.attach_world(world.c_ptr()).has_value());
    scripting.set_ui_context(world.c_ptr(), context);

    ui_probe = ecs_lookup(world.c_ptr(), "SmokeGame.UiProbe");
    ASSERT_NE(ui_probe, 0);
    probe = world.entity();
    const auto initial = UiProbe{};
    ecs_set_id(world.c_ptr(), probe, ui_probe, sizeof(UiProbe), &initial);
  }

  void TearDown() override {
    // the scripts let go of the context's listeners and models before it goes
    scripting.detach_world(world.c_ptr());
    scripting.unload();
    Rml::RemoveContext("dotnet_ui_test");
  }

  auto frame() -> const UiProbe& {
    world.progress(1.0f / 60.0f);
    context->Update();
    return *static_cast<const UiProbe*>(ecs_get_id(world.c_ptr(), probe, ui_probe));
  }

  auto element(const c8* id) -> Rml::Element* {
    auto* document = context->GetDocument(0);
    return document ? document->GetElementById(id) : nullptr;
  }

  static inline std::unique_ptr<ox::App> app = nullptr;
  static inline std::filesystem::path root = {};
  static inline ox::RmlFileInterface files = {};
  static inline NullRenderInterface render = {};

  Rml::Context* context = nullptr;
  flecs::world world;
  ox::DotNetScripting scripting = {};
  flecs::entity_t ui_probe = 0;
  flecs::entity probe = {};
};

TEST_F(DotNetUiTest, LoadsADocumentAndEditsItsElements) {
  frame();
  ASSERT_EQ(context->GetNumDocuments(), 1) << logs_text();
  ASSERT_NE(element("title"), nullptr);
  EXPECT_EQ(element("title")->GetInnerRML(), "Hello");
  EXPECT_TRUE(element("title")->IsClassSet("lit"));
  EXPECT_TRUE(context->GetDocument(0)->IsVisible());
}

TEST_F(DotNetUiTest, BindsADataModel) {
  frame();
  ASSERT_NE(element("score"), nullptr) << logs_text();
  EXPECT_EQ(element("score")->GetInnerRML(), "0");
  EXPECT_EQ(element("label")->GetInnerRML(), "score is 0");

  // the button's data-event-click runs the model's "add" event with 5, which dirties both variables
  element("button")->DispatchEvent(Rml::EventId::Click, {});
  const auto& seen = frame();
  EXPECT_EQ(seen.score, 5);
  EXPECT_EQ(element("score")->GetInnerRML(), "5");
  EXPECT_EQ(element("label")->GetInnerRML(), "score is 5");
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui add 5 from click"), 1) << logs_text();
}

TEST_F(DotNetUiTest, ListensToElementEvents) {
  frame();
  ASSERT_NE(element("button"), nullptr) << logs_text();
  element("button")->DispatchEvent(Rml::EventId::Click, {});
  element("button")->DispatchEvent(Rml::EventId::Click, {});
  EXPECT_EQ(frame().listener_hits, 2);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui listener click on button"), 2);
}

TEST_F(DotNetUiTest, LetsGoOfTheContextWhenTheScriptsUnload) {
  frame();
  ASSERT_TRUE(static_cast<bool>(context->GetDataModel("probe")));
  scripting.unload();

  // the listener and the model are gone, so a click reaches nothing managed
  EXPECT_FALSE(static_cast<bool>(context->GetDataModel("probe")));
  element("button")->DispatchEvent(Rml::EventId::Click, {});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui listener click on button"), 0);
}

TEST_F(DotNetUiTest, ClearsTheStyleSheetAndTemplateCaches) {
  world.entity().set(ox::ScriptComponent{.type_name = "SmokeGame.UiCacheProbe"});
  scripting.start_world(world.c_ptr());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui caches cleared"), 1) << logs_text();

  // a document still loads after the caches were cleared
  frame();
  ASSERT_NE(element("title"), nullptr) << logs_text();
  EXPECT_EQ(element("title")->GetInnerRML(), "Hello");
}
