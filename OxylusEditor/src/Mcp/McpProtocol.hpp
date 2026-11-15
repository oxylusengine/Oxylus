#pragma once

#include <span>
#include <string>
#include <string_view>

#include "Core/Option.hpp"
#include "Core/Types.hpp"
#include "Mcp/McpTool.hpp"

namespace ox {
struct McpCallRequest {
  usize tool_index = 0;
  std::string arguments = "{}";
  bool is_modern = false;
  std::string id = "null";
};

struct McpReply {
  u32 status = 200;
  std::string body = {};
  option<McpCallRequest> call = nullopt;
};

auto mcp_handle_post(
  std::string_view body,
  option<std::string_view> version_header,
  std::span<const McpToolEntry> tools,
  std::string_view server_version
) -> McpReply;

auto mcp_tool_response(const McpCallRequest& call, const McpResult& result) -> std::string;
} // namespace ox
