#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "Utils/SourceReference.hpp"

struct Found {
  std::string path = {};
  u32 line = 0;
  u32 column = 0;

  auto operator==(const Found&) const -> bool = default;
};

static auto find(std::string_view text) -> std::vector<Found> {
  auto found = std::vector<Found>();
  for (const auto& reference : ox::find_source_references(text)) {
    found.push_back({.path = std::string(reference.path), .line = reference.line, .column = reference.column});
  }

  return found;
}

TEST(SourceReference, ReadsMsbuildDiagnostics) {
  EXPECT_EQ(
    find(R"(C:\My Games\Rogylus\Assets\Scripts\RogylusGame.cs(145,22): error CS0103: The name 'x' does not exist)"),
    (std::vector<Found>{{R"(C:\My Games\Rogylus\Assets\Scripts\RogylusGame.cs)", 145, 22}})
  );
  EXPECT_EQ(
    find("/home/dev/game/Logic.cs(12): warning CS0168: unused"),
    (std::vector<Found>{{"/home/dev/game/Logic.cs", 12, 0}})
  );
}

TEST(SourceReference, ReadsEveryStackFrameWithALocation) {
  const auto text = std::string(
    "Rogylus.RogylusGame.OnUpdate threw and the script is disabled: System.InvalidOperationException: boom\r\n"
    "   at Rogylus.Rig.Steer(Single amount) in C:\\game\\Assets\\Scripts\\Rig.cs:line 88\r\n"
    "   at Rogylus.RogylusGame.OnUpdate(Single deltaTime) in C:\\game\\Assets\\Scripts\\RogylusGame.cs:line 145\r\n"
    "   at Oxylus.Hosting.Scenes.WorldUpdate(IntPtr world, Single deltaTime)\r\n"
  );
  EXPECT_EQ(
    find(text),
    (std::vector<Found>{
      {R"(C:\game\Assets\Scripts\Rig.cs)", 88, 0},
      {R"(C:\game\Assets\Scripts\RogylusGame.cs)", 145, 0},
    })
  );
}

TEST(SourceReference, IgnoresTextThatOnlyLooksLikeAReference) {
  EXPECT_TRUE(find("Loaded C# scripts Rogylus.dll").empty());
  EXPECT_TRUE(find("Couldn't create the window (5): no display").empty());
  EXPECT_TRUE(find("C:\\game\\Logic.cs(12,x): error").empty());
  EXPECT_TRUE(find("Logic.cs(12, 5) is where it went wrong").empty());
  EXPECT_TRUE(find("   at Game.Logic.Update() in :line 12").empty());
  EXPECT_TRUE(find("   at Game.Logic.Update() in C:\\game\\Logic.cs:line 12a").empty());
  EXPECT_TRUE(find("").empty());
}
