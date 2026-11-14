#include "Mcp/McpInstall.hpp"

#include <SDL3/SDL_filesystem.h>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <system_error>
#include <tracy/Tracy.hpp>

#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Utils/Log.hpp"
#include "Utils/StringUtils.hpp"

namespace ox {
constexpr static std::string_view AGENTS_HEADING = "## Oxylus editor (MCP)";

constexpr static std::string_view AGENTS_SECTION =
  R"(## Oxylus editor (MCP)

The Oxylus editor exposes an MCP server while it has this project open.
- Connect through `.mcp.json` (stdio, `oxmcp --project .`) or the editor's `Tools > MCP` menu.
- Start the editor with `OxylusEditor --project <file.oxproj>` or `oxmcp --launch --project .`.
- Call `editor_state` first, then read `editor_guide` before a multi-step job.
- Edit scenes, components and assets through the tools so the editor stays in sync and undo works.
- Move and rename assets through the `file_*` tools so their sidecars follow.
- Plain text the editor does not own, such as C# sources, can be edited on disk.
)";

static auto read_text(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

static auto write_text(const std::filesystem::path& path, std::string_view text, std::ios::openmode mode) -> bool {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);

  std::ofstream file(path, std::ios::binary | mode);
  if (!file) {
    return false;
  }

  file << text;
  return file.good();
}

static auto report_written(std::string_view what, bool is_ok) -> void {
  if (is_ok) {
    OX_LOG_INFO("MCP install: wrote {}", what);
  } else {
    OX_LOG_ERROR("MCP install: could not write {}", what);
  }
}

auto mcp_bridge_path() -> std::filesystem::path {
  const auto* base = SDL_GetBasePath();
  const auto directory = base != nullptr ? std::filesystem::path(base) : std::filesystem::current_path();
#ifdef _WIN32
  return directory / "oxmcp.exe";
#else
  return directory / "oxmcp";
#endif
}

auto mcp_claude_command(u16 port) -> std::string {
  return fmt::format("claude mcp add --transport http oxylus http://127.0.0.1:{}/mcp", port);
}

auto mcp_stdio_config(const std::filesystem::path& project_dir) -> std::string {
  const auto bridge = StringUtils::escape_str(mcp_bridge_path().generic_string());
  const auto project = StringUtils::escape_str(project_dir.generic_string());
  return fmt::format(R"({{"mcpServers":{{"oxylus":{{"command":"{}","args":["--project","{}"]}}}}}})", bridge, project);
}

auto install_agent_files(const std::filesystem::path& project_dir) -> void {
  ZoneScoped;

  const auto mcp_json = project_dir / ".mcp.json";
  if (std::filesystem::exists(mcp_json)) {
    OX_LOG_INFO("MCP install: skipped {}, it already exists", mcp_json.string());
  } else {
    const auto bridge = StringUtils::escape_str(mcp_bridge_path().generic_string());
    const auto text = fmt::format(
                        R"({{"mcpServers":{{"oxylus":{{"command":"{}","args":["--project","."]}}}}}})",
                        bridge
                      ) +
                      "\n";
    report_written(mcp_json.string(), write_text(mcp_json, text, std::ios::trunc));
  }

  const auto skill_target = project_dir / ".claude/skills/oxylus-editor/SKILL.md";
  if (std::filesystem::exists(skill_target)) {
    OX_LOG_INFO("MCP install: skipped {}, it already exists", skill_target.string());
  } else {
    const auto skill_source = App::get_vfs().resolve_physical_dir(VFS::APP_DIR, "Mcp/skills/oxylus-editor/SKILL.md");
    if (!std::filesystem::is_regular_file(skill_source)) {
      OX_LOG_ERROR("MCP install: the installed skill {} is missing", skill_source.string());
    } else {
      report_written(skill_target.string(), write_text(skill_target, read_text(skill_source), std::ios::trunc));
    }
  }

  const auto agents = project_dir / "AGENTS.md";
  const auto existing = std::filesystem::exists(agents) ? read_text(agents) : std::string();
  if (existing.contains(AGENTS_HEADING)) {
    OX_LOG_INFO("MCP install: skipped {}, it already has the Oxylus editor section", agents.string());
  } else {
    std::string text;
    if (!existing.empty()) {
      text += existing.ends_with('\n') ? "\n" : "\n\n";
    }

    text += AGENTS_SECTION;
    report_written(agents.string(), write_text(agents, text, std::ios::app));
  }
}
} // namespace ox
