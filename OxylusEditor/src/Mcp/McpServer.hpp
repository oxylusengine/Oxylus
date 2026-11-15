#pragma once

#include <ankerl/unordered_dense.h>
#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "Core/Types.hpp"
#include "Core/UUID.hpp"
#include "Mcp/McpTool.hpp"

namespace ox {
class McpHttp;
struct McpCallRequest;

class McpServer {
public:
  McpServer();
  ~McpServer();

  McpServer(const McpServer&) = delete;
  auto operator=(const McpServer&) -> McpServer& = delete;

  // immutable once the server has started
  std::vector<McpToolEntry> tools = {};
  u16 bound_port = 0;
  bool is_running = false;
  std::string last_error = {};
  // asset_load refs taken on behalf of agents, given back in stop
  ankerl::unordered_dense::map<UUID, u32> held_assets = {};

  auto add_tool(this McpServer& self, McpTool tool) -> void;

  auto start(this McpServer& self, u16 port, std::string token) -> std::expected<u16, std::string>;
  auto stop(this McpServer& self) -> void;

  auto pump(this McpServer& self, Editor& editor) -> void;

  // called from the HTTP thread, blocks until the main thread has answered or the cap passes
  auto submit_and_wait(this McpServer& self, McpCallRequest request, std::chrono::milliseconds cap) -> std::string;

  struct State;

private:
  std::unique_ptr<McpHttp> http;
  std::unique_ptr<State> state;
};
} // namespace ox
