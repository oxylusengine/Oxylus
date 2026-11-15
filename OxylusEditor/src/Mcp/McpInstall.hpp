#pragma once

#include <filesystem>
#include <string>

#include "Core/Types.hpp"

namespace ox {
auto mcp_bridge_path() -> std::filesystem::path;
auto mcp_claude_command(u16 port) -> std::string;
auto mcp_stdio_config(const std::filesystem::path& project_dir) -> std::string;
// writes `.mcp.json`, the skill and an AGENTS.md section into the project, each only when absent
auto install_agent_files(const std::filesystem::path& project_dir) -> void;
} // namespace ox
