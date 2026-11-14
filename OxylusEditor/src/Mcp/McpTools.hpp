#pragma once

namespace ox {
class McpServer;

auto register_editor_tools(McpServer& server) -> void;
auto register_entity_tools(McpServer& server) -> void;
auto register_scene_tools(McpServer& server) -> void;
auto register_project_tools(McpServer& server) -> void;
auto register_settings_tools(McpServer& server) -> void;
auto register_asset_tools(McpServer& server) -> void;
auto register_file_tools(McpServer& server) -> void;
auto register_script_tools(McpServer& server) -> void;
auto register_view_tools(McpServer& server) -> void;
} // namespace ox
