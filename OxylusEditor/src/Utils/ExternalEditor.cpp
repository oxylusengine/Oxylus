#include "Utils/ExternalEditor.hpp"

#include <algorithm>
#include <cctype>
#include <expected>
#include <span>
#include <string>

#include "Core/App.hpp"
#include "Editor.hpp"
#include "Memory/Stack.hpp"
#include "OS/OS.hpp"
#include "Project/Project.hpp"
#include "Utils/CommandTemplate.hpp"
#include "Utils/Log.hpp"

namespace ox {
static auto spawn_tokens(std::span<const std::string> tokens) -> std::expected<void, std::string> {
  memory::ScopedStack stack;

  auto args = stack.alloc<std::string_view>(tokens.size());
  std::ranges::copy(tokens, args.begin());

  return os::spawn_detached(args);
}

auto open_in_external_editor(const std::filesystem::path& file, u32 line, u32 column) -> void {
  ZoneScoped;

  auto& editor = App::mod<Editor>();
  const auto command = editor.editor_cvar.cvar_external_editor.get();
  const auto is_blank = std::ranges::all_of(command, [](c8 c) { return std::isspace(static_cast<u8>(c)) != 0; });
  if (is_blank) {
    os::open_file_externally(file);
    return;
  }

  option<std::filesystem::path> root = nullopt;
  if (editor.active_project) {
    root = editor.active_project->get_project_directory();
  }

  auto tokens = expand_command_template(command, {.file = file, .line = line, .column = column, .root = root});
  if (!tokens) {
    OX_LOG_ERROR("{}", tokens.error());
    return;
  }

  auto result = spawn_tokens(*tokens);
#ifdef OX_PLATFORM_WINDOWS
  if (!result && !std::filesystem::path((*tokens)[0]).has_extension()) {
    (*tokens)[0] += ".cmd";
    result = spawn_tokens(*tokens);
  }
#endif

  if (!result) {
    OX_LOG_ERROR("{}", result.error());
  }
}
} // namespace ox
