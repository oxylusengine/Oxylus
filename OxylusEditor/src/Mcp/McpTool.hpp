#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Core/Option.hpp"
#include "Core/Types.hpp"

namespace ox {
class Editor;
struct McpArgs;

struct McpResult {
  std::string text = {};
  std::string image_png = {};
  bool is_error = false;
  std::function<option<McpResult>()> poll = {};

  static auto ok(std::string text) -> McpResult { return McpResult{.text = std::move(text)}; }

  static auto error(std::string text) -> McpResult { return McpResult{.text = std::move(text), .is_error = true}; }

  // the arguments die when the handler returns, so poll must not capture McpArgs or views into them
  static auto pending(std::function<option<McpResult>()> poll) -> McpResult {
    return McpResult{.poll = std::move(poll)};
  }

  static auto image(std::string png, std::string caption) -> McpResult {
    return McpResult{.text = std::move(caption), .image_png = std::move(png)};
  }
};

struct McpTool {
  std::string name = {};
  std::string title = {};
  std::string description = {};
  std::string input_schema = {};
  bool is_read_only = false;
  bool is_destructive = false;
  bool is_idempotent = false;
  u32 timeout_ms = 30000;
  std::function<McpResult(Editor&, const McpArgs&)> handler = {};
};

struct McpToolEntry {
  McpTool tool = {};
  std::vector<std::string> arg_names = {};
};
} // namespace ox
