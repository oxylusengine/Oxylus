#include <gtest/gtest.h>

#include "Utils/CommandTemplate.hpp"

using Tokens = std::vector<std::string>;

static auto make_args(ox::option<std::filesystem::path> root = ox::nullopt) -> ox::CommandTemplateArgs {
  return {.file = "/home/dev/code bases/station/items.dm", .line = 42, .column = 7, .root = std::move(root)};
}

TEST(CommandTemplate, VSCodeOpensTheFileAtItsLocation) {
  const auto result = ox::expand_command_template("code --goto {file}:{line}:{column}", make_args());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, (Tokens{"code", "--goto", "/home/dev/code bases/station/items.dm:42:7"}));
}

TEST(CommandTemplate, RootPlaceholderWorksQuotedAndUnquoted) {
  const auto root = std::filesystem::path("/home/dev/code bases/station");
  for (const auto* command : {
         "code {root} --goto {file}:{line}:{column}",
         "code \"{root}\" --goto {file}:{line}:{column}",
       }) {
    const auto result = ox::expand_command_template(command, make_args(root));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(
      *result,
      (Tokens{"code", "/home/dev/code bases/station", "--goto", "/home/dev/code bases/station/items.dm:42:7"})
    );
  }
}

TEST(CommandTemplate, RootPlaceholderRequiresAProject) {
  const auto result = ox::expand_command_template("code {root} --goto {file}:{line}:{column}", make_args());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), "external editor command uses {root}, but no project is open");
}

TEST(CommandTemplate, QuotedProgramsAndFixedArgumentsArePreserved) {
  const auto result = ox::expand_command_template(
    R"("C:\Program Files\Editor\editor.exe" --line {line} {file})",
    make_args()
  );
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(
    *result,
    (Tokens{R"(C:\Program Files\Editor\editor.exe)", "--line", "42", "/home/dev/code bases/station/items.dm"})
  );
}

TEST(CommandTemplate, QuotesCanAppearMidTokenAndEmptyQuotesYieldAToken) {
  const auto result = ox::expand_command_template(R"(code --x="a b" '' {file})", make_args());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, (Tokens{"code", "--x=a b", "", "/home/dev/code bases/station/items.dm"}));
}

TEST(CommandTemplate, CommandsMustBeValidAndIncludeTheFile) {
  EXPECT_EQ(ox::expand_command_template("", make_args()).error(), "external editor command is empty");
  EXPECT_EQ(
    ox::expand_command_template("code \"{file}", make_args()).error(),
    "external editor command has invalid quoting"
  );
  EXPECT_EQ(
    ox::expand_command_template("code --goto nowhere", make_args()).error(),
    "external editor command must contain {file}"
  );
}
