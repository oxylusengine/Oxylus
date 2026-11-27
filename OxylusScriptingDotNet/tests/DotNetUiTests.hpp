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
  i32 command = 0;
  i32 count = 0;
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
  <div id="outer">
    <div id="keyed"></div>
    <input id="field" type="text"/>
    <button id="doomed">doomed</button>
  </div>
  <div id="inert" style="focus: none"></div>
  <div id="box" class="boxed" style="display: block; width: 120px"></div>
  <div id="list"><p class="item" data-for="item : items">{{item.name}} x{{item.count}}</p></div>
  <span id="size">{{items.size}}</span>
  <div id="names"><i class="name" data-for="n : names">{{n}};</i></div>
  <div id="bulk"></div>
</body>
</rml>
)rml";
    document.close();

    const auto font = std::filesystem::path(OX_ENGINE_DIR) / "OxylusEditor" / "Assets" / "Fonts" /
                      "FiraSans-Regular.ttf";
    std::filesystem::create_directories(root / "Fonts");
    std::filesystem::copy_file(font, root / "Fonts" / "probe.ttf", std::filesystem::copy_options::overwrite_existing);
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

  auto run(i32 command) -> const UiProbe& {
    auto current = *static_cast<const UiProbe*>(ecs_get_id(world.c_ptr(), probe, ui_probe));
    current.command = command;
    ecs_set_id(world.c_ptr(), probe, ui_probe, sizeof(UiProbe), &current);
    return frame();
  }

  auto texts(const c8* selector) -> std::vector<std::string> {
    auto matches = Rml::ElementList{};
    context->GetDocument(0)->QuerySelectorAll(matches, selector);

    auto found = std::vector<std::string>{};
    for (auto* match : matches) {
      // the data-for element itself stays behind empty, as the template
      if (!match->GetInnerRML().empty()) {
        found.push_back(match->GetInnerRML());
      }
    }

    return found;
  }

  auto key_event(Rml::Input::KeyIdentifier key, bool is_ctrl, bool is_shift) -> Rml::Dictionary {
    auto parameters = Rml::Dictionary{};
    parameters["key_identifier"] = static_cast<int>(key);
    parameters["ctrl_key"] = static_cast<int>(is_ctrl);
    parameters["shift_key"] = static_cast<int>(is_shift);
    parameters["alt_key"] = 0;
    parameters["meta_key"] = 0;
    return parameters;
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

TEST_F(DotNetUiTest, LoadsAFontFaceAndRejectsAMissingOne) {
  world.entity().set(ox::ScriptComponent{.type_name = "SmokeGame.UiFontProbe"});
  scripting.start_world(world.c_ptr());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui missing font threw"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui missing font loaded"), 0);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui font loaded"), 1) << logs_text();
}

TEST_F(DotNetUiTest, RendersListsOfRecordsAndValues) {
  frame();
  ASSERT_NE(element("list"), nullptr) << logs_text();
  EXPECT_EQ(texts(".item"), (std::vector<std::string>{"apple x3", "pear x5"})) << logs_text();
  EXPECT_EQ(element("size")->GetInnerRML(), "2");
  EXPECT_EQ(texts(".name"), (std::vector<std::string>{"red;", "green;"}));

  run(1);
  EXPECT_EQ(texts(".item"), (std::vector<std::string>{"apple x3", "pear x5", "fig x9"})) << logs_text();
  EXPECT_EQ(element("size")->GetInnerRML(), "3");
  EXPECT_EQ(texts(".name"), (std::vector<std::string>{"red;", "green;", "blue;"}));

  run(2);
  EXPECT_EQ(texts(".item"), (std::vector<std::string>{"pear x5", "fig x9"}));
  EXPECT_EQ(element("size")->GetInnerRML(), "2");
  EXPECT_TRUE(texts(".name").empty());
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetUiTest, RejectsListsOfUnsupportedTypes) {
  frame();
  run(12);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui bad list True field True"), 1) << logs_text();
}

TEST_F(DotNetUiTest, HandsKeyEventDataToListeners) {
  frame();
  ASSERT_NE(element("keyed"), nullptr) << logs_text();

  captured_logs.clear();
  EXPECT_TRUE(element("keyed")->DispatchEvent(Rml::EventId::Keydown, key_event(Rml::Input::KI_A, false, false)));
  EXPECT_EQ(
    count_logs(
      ox::OxLogLevel::Info,
      "ui key A ctrl False shift False alt False meta False raw 12 fallback 7 text none"
    ),
    1
  ) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui field second"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui field late"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui outer keydown from keyed"), 1);

  captured_logs.clear();
  EXPECT_FALSE(element("keyed")->DispatchEvent(Rml::EventId::Keydown, key_event(Rml::Input::KI_ESCAPE, true, false)));
  EXPECT_EQ(
    count_logs(
      ox::OxLogLevel::Info,
      "ui key Escape ctrl True shift False alt False meta False raw 81 fallback 7 text none"
    ),
    1
  ) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui field second"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui field late"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui outer keydown from keyed"), 0);

  captured_logs.clear();
  EXPECT_FALSE(element("keyed")->DispatchEvent(Rml::EventId::Keydown, key_event(Rml::Input::KI_F1, false, true)));
  EXPECT_EQ(
    count_logs(
      ox::OxLogLevel::Info,
      "ui key F1 ctrl False shift True alt False meta False raw 107 fallback 7 text none"
    ),
    1
  ) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui field second"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui field late"), 0);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui outer keydown from keyed"), 0);
}

TEST_F(DotNetUiTest, HandsMouseAndWheelDataToListeners) {
  frame();
  ASSERT_NE(element("box"), nullptr) << logs_text();

  auto press = Rml::Dictionary{};
  press["button"] = 1;
  element("box")->DispatchEvent(Rml::EventId::Mousedown, press);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui mousedown button 1 bool True"), 1) << logs_text();

  auto scroll = Rml::Dictionary{};
  scroll["wheel_delta_x"] = 0.5f;
  scroll["wheel_delta_y"] = -2.0f;
  element("box")->DispatchEvent(Rml::EventId::Mousescroll, scroll);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui wheel 0.5 -2 number -2"), 1) << logs_text();
}

TEST_F(DotNetUiTest, FocusesAndBlursElements) {
  frame();
  ASSERT_NE(element("field"), nullptr) << logs_text();
  EXPECT_NE(context->GetFocusElement(), element("field"));

  run(5);
  EXPECT_EQ(context->GetFocusElement(), element("field")) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui focus True"), 1);

  run(6);
  EXPECT_NE(context->GetFocusElement(), element("field"));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui blur"), 1);

  run(13);
  EXPECT_NE(context->GetFocusElement(), element("inert"));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui focus False"), 1) << logs_text();
}

TEST_F(DotNetUiTest, AppendsAndRemovesElements) {
  frame();
  ASSERT_EQ(element("made"), nullptr);

  run(3);
  auto* made = element("made");
  ASSERT_NE(made, nullptr) << logs_text();
  EXPECT_EQ(made->GetTagName(), "div");
  EXPECT_EQ(made->GetInnerRML(), "made it");
  EXPECT_TRUE(made->IsClassSet("fresh"));
  EXPECT_EQ(made->GetParentNode(), element("outer"));
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui appended True parent True"), 1) << logs_text();

  // from a system, nothing is dispatching, so it is freed right away
  run(14);
  EXPECT_EQ(element("made"), nullptr) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetUiTest, RemovesAnElementFromItsOwnListener) {
  frame();
  ASSERT_NE(element("doomed"), nullptr) << logs_text();
  const auto children = element("outer")->GetNumChildren();

  element("doomed")->DispatchEvent(Rml::EventId::Click, {});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui doomed click"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui doomed removed, still doomed"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui outer click from doomed"), 1) << logs_text();
  EXPECT_EQ(element("doomed"), nullptr);
  EXPECT_EQ(element("outer")->GetNumChildren(), children - 1);

  run(9);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui stale True True True True True True True True"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetUiTest, RefusesToRemoveADocument) {
  frame();
  run(8);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui root remove True"), 1) << logs_text();
  EXPECT_EQ(context->GetNumDocuments(), 1);
}

TEST_F(DotNetUiTest, QueriesAllMatches) {
  frame();
  const auto& seen = run(4);
  EXPECT_EQ(seen.count, 12) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui queryall 12 parented True none 0 one 1"), 1) << logs_text();

  auto bulk = Rml::ElementList{};
  element("bulk")->QuerySelectorAll(bulk, ".bulk");
  EXPECT_EQ(bulk.size(), 12);
}

TEST_F(DotNetUiTest, ReadsParentPropertyAndClass) {
  frame();
  run(7);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui parent field True outer True root False"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui property width 120px display block"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui hasclass True False"), 1) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui property missing True"), 1) << logs_text();
}

TEST_F(DotNetUiTest, ClicksAndScrollsElements) {
  frame();
  const auto& clicked = run(10);
  EXPECT_EQ(clicked.score, 5) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui add 5 from click"), 1) << logs_text();
  EXPECT_EQ(clicked.listener_hits, 1);

  run(11);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "ui scrolled"), 1) << logs_text();
}
