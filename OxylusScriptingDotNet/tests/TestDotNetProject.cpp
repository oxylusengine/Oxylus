#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

#include "DotNetTestHelpers.hpp"
#include "Scripting/DotNetProject.hpp"

static auto write_text(const std::filesystem::path& path, std::string_view text) -> void {
  std::filesystem::create_directories(path.parent_path());
  auto file = std::ofstream(path, std::ios::binary);
  file << text;
}

TEST(DotNetProjectTest, ParsesMsbuildDiagnostics) {
  const auto output = std::string_view(
    "  Determining projects to restore...\r\n"
    "C:\\Game\\Assets\\Player.cs(12,9): error CS1002: ; expected [C:\\Game\\Scripts\\Game.csproj]\r\n"
    "C:\\Game\\Assets\\Player.cs(12,9): error CS1002: ; expected [C:\\Game\\Scripts\\Game.csproj]\r\n"
    "C:\\Game\\Scripts\\Loader.cs(3,20): warning IL2057: Unrecognized value passed to 'type' [C:\\Game.csproj]\n"
    "MSBUILD : error MSB1009: Project file does not exist.\n"
    "Build FAILED.\n"
  );

  const auto diagnostics = ox::parse_msbuild_output(output);
  ASSERT_EQ(diagnostics.size(), 3);

  EXPECT_EQ(diagnostics[0].severity, ox::DotNetDiagnosticSeverity::Error);
  EXPECT_EQ(diagnostics[0].file, "C:\\Game\\Assets\\Player.cs");
  EXPECT_EQ(diagnostics[0].line, 12);
  EXPECT_EQ(diagnostics[0].column, 9);
  EXPECT_EQ(diagnostics[0].code, "CS1002");
  EXPECT_EQ(diagnostics[0].message, "; expected");

  EXPECT_EQ(diagnostics[1].severity, ox::DotNetDiagnosticSeverity::Warning);
  EXPECT_EQ(diagnostics[1].code, "IL2057");
  EXPECT_EQ(diagnostics[1].message, "Unrecognized value passed to 'type'");

  EXPECT_TRUE(diagnostics[2].file.empty());
  EXPECT_EQ(diagnostics[2].code, "MSB1009");
  EXPECT_EQ(diagnostics[2].message, "Project file does not exist.");
}

TEST(DotNetProjectTest, MakesCSharpIdentifiers) {
  EXPECT_EQ(ox::to_csharp_identifier("My Game 2"), "MyGame2");
  EXPECT_EQ(ox::to_csharp_identifier("space-shooter"), "SpaceShooter");
  EXPECT_EQ(ox::to_csharp_identifier("2048"), "_2048");
  EXPECT_EQ(ox::to_csharp_identifier("!!"), "Game");
}

// what the editor does with a project: write the csproj and props, build into the cache, load it next to its own
// Oxylus.Scripting
class DotNetProjectBuildTest : public ::testing::Test {
protected:
  void SetUp() override {
    captured_logs.clear();
    root = std::filesystem::temp_directory_path() / "ox_dotnet_project_test";
    auto error = std::error_code();
    std::filesystem::remove_all(root, error);

    const auto created = ox::create_scripts_project(root / "Scripts", "Test Game");
    ASSERT_TRUE(created.has_value()) << created.error();
    csproj = *created;
    const auto written = ox::write_scripts_props(root / "Scripts", binary_dir() / "managed", root / "Assets");
    ASSERT_TRUE(written.has_value()) << written.error();
  }

  void TearDown() override {
    scripting.unload();
    // a build server may still hold something in obj, a leftover is cleared by the next run
    auto error = std::error_code();
    std::filesystem::remove_all(root, error);
  }

  auto output_dir() const -> std::filesystem::path { return root / ".oxeditor" / "managed"; }

  std::filesystem::path root = {};
  std::filesystem::path csproj = {};
  ox::DotNetScripting scripting = {};
};

TEST_F(DotNetProjectBuildTest, BuildsAndLoadsAGeneratedProject) {
  EXPECT_EQ(csproj.filename(), "TestGame.csproj");
  write_text(
    root / "Assets" / "Behaviours" / "Mover.cs",
    ox::csharp_script_template(ox::CSharpScriptKind::EntityBehaviour, "TestGame", "Mover")
  );
  write_text(
    root / "Scripts" / "Level.cs",
    ox::csharp_script_template(ox::CSharpScriptKind::SceneBehaviour, "TestGame", "Level")
  );

  const auto result = ox::build_scripts_project(csproj, output_dir());
  ASSERT_TRUE(result.is_success) << result.output;
  const auto assembly = ox::scripts_assembly_path(csproj, output_dir());
  ASSERT_TRUE(std::filesystem::exists(assembly)) << assembly.string();
  // the editor's copy is the one that loads, the build mustn't bring its own
  EXPECT_FALSE(std::filesystem::exists(output_dir() / "Oxylus.Scripting.dll"));

  const auto loaded = scripting.load(
    {.kind = ox::DotNetHostKind::HostFxr, .path = assembly, .scripting_directory = binary_dir() / "managed"},
    capturing_native_api()
  );
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Warn), 0);

  const auto& types = scripting.script_types;
  ASSERT_EQ(types.size(), 2);
  EXPECT_EQ(types[0].name, "TestGame.Level");
  EXPECT_EQ(types[0].kind, ox::OxScriptKind::SceneBehaviour);
  EXPECT_EQ(types[1].name, "TestGame.Mover");
  EXPECT_EQ(types[1].kind, ox::OxScriptKind::EntityBehaviour);
}

TEST_F(DotNetProjectBuildTest, ReportsCompileErrorsWithTheirLocation) {
  write_text(root / "Assets" / "Broken.cs", "namespace TestGame;\n\npublic class Broken\n{\n    int x = \n}\n");

  const auto result = ox::build_scripts_project(csproj, output_dir());
  EXPECT_FALSE(result.is_success);
  const auto error = std::ranges::find_if(result.diagnostics, [](const ox::DotNetDiagnostic& diagnostic) {
    return diagnostic.severity == ox::DotNetDiagnosticSeverity::Error && diagnostic.code.starts_with("CS");
  });
  ASSERT_NE(error, result.diagnostics.end()) << result.output;
  EXPECT_TRUE(error->file.ends_with("Broken.cs")) << error->file;
  EXPECT_EQ(error->line, 5);
}

TEST_F(DotNetProjectBuildTest, FailsCodeThatWouldBreakNativeAot) {
  write_text(
    root / "Scripts" / "Loader.cs",
    "namespace TestGame;\n\npublic static class Loader\n{\n"
    "    public static System.Type? Find(string name) => System.Type.GetType(name);\n}\n"
  );

  const auto result = ox::build_scripts_project(csproj, output_dir());
  EXPECT_FALSE(result.is_success);
  const auto is_aot_error = std::ranges::any_of(result.diagnostics, [](const ox::DotNetDiagnostic& diagnostic) {
    return diagnostic.severity == ox::DotNetDiagnosticSeverity::Error && diagnostic.code.starts_with("IL");
  });
  EXPECT_TRUE(is_aot_error) << result.output;
}
