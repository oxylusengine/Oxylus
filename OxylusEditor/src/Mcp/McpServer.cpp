#include "Mcp/McpServer.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <simdjson.h>
#include <tracy/Tracy.hpp>

#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

#include "Asset/AssetManager.hpp"
#include "Core/App.hpp"
#include "Core/Version.hpp"
#include "Editor.hpp"
#include "Mcp/McpArgs.hpp"
#include "Mcp/McpHttp.hpp"
#include "Mcp/McpProtocol.hpp"
#include "Utils/JsonWriter.hpp"

namespace ox {
struct McpPendingCall {
  McpCallRequest request = {};
  std::promise<std::string> promise = {};
  std::chrono::steady_clock::time_point started = {};
  std::function<option<McpResult>()> poll = {};
  u32 timeout_ms = 0;
};

struct McpServer::State {
  std::mutex mutex = {};
  std::vector<McpPendingCall> queue = {};
  bool is_accepting = false;

  std::vector<McpPendingCall> pending = {};
  std::string token = {};

  bool has_applied = false;
  bool applied_is_enabled = false;
  i32 applied_port = 0;
  std::string applied_token = {};

  std::filesystem::path discovery_path = {};
  u16 discovery_port = 0;
};

constexpr static std::string_view DISCOVERY_FILE = "mcp.json";
constexpr static std::string_view DISCOVERY_DIR = ".oxeditor";

static auto current_pid() -> i64 {
#ifdef _WIN32
  return _getpid();
#else
  return getpid();
#endif
}

static auto fulfil(McpPendingCall& call, const McpResult& result) -> void {
  call.promise.set_value(mcp_tool_response(call.request, result));
}

static auto port_override() -> option<u16> {
  const auto* app = App::get();
  if (app == nullptr) {
    return nullopt;
  }

  const auto& args = app->get_command_line_args();
  const auto index = args.get_index("--mcp-port");
  if (!index.has_value()) {
    return nullopt;
  }

  const auto value = args.get(index.value() + 1);
  if (!value.has_value()) {
    return nullopt;
  }

  u32 parsed = 0;
  const auto& text = value->arg_str;
  const auto parse_result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (parse_result.ec != std::errc{} || parsed == 0 || parsed > 65535) {
    return nullopt;
  }

  return static_cast<u16>(parsed);
}

static auto remove_discovery(McpServer::State& state) -> void {
  if (state.discovery_path.empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::remove(state.discovery_path, error);
  state.discovery_path.clear();
  state.discovery_port = 0;
}

static auto write_discovery(const std::filesystem::path& path, u16 port, bool is_token_required) -> void {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);

  JsonWriter writer;
  writer.begin_obj();
  writer.key("url");
  writer.str(fmt::format("http://127.0.0.1:{}/mcp", port));
  writer["port"] = static_cast<u32>(port);
  writer["pid"] = current_pid();
  writer["token_required"] = is_token_required;
  writer.key("editor_version");
  writer.str(engine_build_info().full_version);
  writer.end_obj();

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << writer.stream.str() << '\n';
}

McpServer::McpServer() : state(std::make_unique<State>()) {}

McpServer::~McpServer() { stop(); }

auto McpServer::add_tool(this McpServer& self, McpTool tool) -> void {
  ZoneScoped;

  McpToolEntry entry;
  if (!tool.input_schema.empty()) {
    simdjson::dom::parser parser;
    simdjson::dom::element schema;
    simdjson::dom::object properties;
    if (!parser.parse(tool.input_schema).get(schema) && !schema["properties"].get(properties)) {
      for (const auto& field : properties) {
        entry.arg_names.emplace_back(field.key);
      }
    }
  }

  entry.tool = std::move(tool);
  self.tools.emplace_back(std::move(entry));
}

auto McpServer::start(this McpServer& self, u16 port, std::string token) -> std::expected<u16, std::string> {
  ZoneScoped;

  if (self.is_running) {
    self.stop();
  }

  {
    std::unique_lock lock(self.state->mutex);
    self.state->is_accepting = true;
  }

  self.state->token = token;
  self.http = std::make_unique<McpHttp>();

  const auto started = self.http->start(self, port, std::move(token), engine_build_info().version);
  if (!started) {
    std::unique_lock lock(self.state->mutex);
    self.state->is_accepting = false;
    self.http.reset();
    return std::unexpected(started.error());
  }

  self.bound_port = started.value();
  self.is_running = true;
  self.last_error.clear();
  return started.value();
}

auto McpServer::stop(this McpServer& self) -> void {
  ZoneScoped;

  if (!self.held_assets.empty()) {
    auto& asset_man = App::mod<AssetManager>();
    for (const auto& [uuid, count] : self.held_assets) {
      for (u32 i = 0; i < count; i++) {
        asset_man.unload_asset(uuid);
      }
    }

    self.held_assets.clear();
  }

  if (!self.http && !self.is_running) {
    return;
  }

  std::vector<McpPendingCall> queued;
  {
    std::unique_lock lock(self.state->mutex);
    self.state->is_accepting = false;
    queued.swap(self.state->queue);
  }

  const auto stopped = McpResult::error("the editor MCP server is stopping");
  for (auto& call : queued) {
    fulfil(call, stopped);
  }

  for (auto& call : self.state->pending) {
    fulfil(call, stopped);
  }
  self.state->pending.clear();

  if (self.http) {
    self.http->stop();
    self.http.reset();
  }

  remove_discovery(*self.state);

  self.is_running = false;
  self.bound_port = 0;
}

auto McpServer::submit_and_wait(this McpServer& self, McpCallRequest request, std::chrono::milliseconds cap)
  -> std::string {
  ZoneScoped;

  McpPendingCall call;
  call.request = request;
  call.timeout_ms = self.tools[request.tool_index].tool.timeout_ms;
  auto future = call.promise.get_future();

  {
    std::unique_lock lock(self.state->mutex);
    if (!self.state->is_accepting) {
      return mcp_tool_response(request, McpResult::error("the editor MCP server is stopping"));
    }

    self.state->queue.emplace_back(std::move(call));
  }

  if (future.wait_for(cap) == std::future_status::timeout) {
    return mcp_tool_response(request, McpResult::error("the editor did not answer, it may be busy or frozen"));
  }

  return future.get();
}

static auto run_call(McpServer& server, Editor& editor, McpPendingCall& call) -> bool {
  const auto& tool = server.tools[call.request.tool_index].tool;

  simdjson::dom::parser parser;
  simdjson::dom::element root;
  if (parser.parse(call.request.arguments).get(root)) {
    fulfil(call, McpResult::error("could not parse the arguments"));
    return false;
  }

  auto args = McpArgs::from(root);
  if (!args) {
    fulfil(call, McpResult::error(args.error()));
    return false;
  }

  auto result = tool.handler(editor, *args);
  if (!result.poll) {
    fulfil(call, result);
    return false;
  }

  call.poll = std::move(result.poll);
  call.timeout_ms = tool.timeout_ms;
  call.started = std::chrono::steady_clock::now();
  return true;
}

static auto apply_settings(McpServer& server, McpServer::State& state, Editor& editor) -> void {
  const auto is_enabled = editor.editor_cvar.cvar_mcp_enabled.as_bool();
  const auto port = editor.editor_cvar.cvar_mcp_port.get();
  auto token = editor.editor_cvar.cvar_mcp_token.get();

  const auto is_first = !state.has_applied;
  if (
    !is_first && is_enabled == state.applied_is_enabled && port == state.applied_port && token == state.applied_token
  ) {
    return;
  }

  state.has_applied = true;
  state.applied_is_enabled = is_enabled;
  state.applied_port = port;
  state.applied_token = token;

  if (server.is_running) {
    server.stop();
  }

  server.last_error.clear();
  if (!is_enabled) {
    return;
  }

  auto wanted_port = static_cast<u16>(std::clamp(port, 1, 65535));
  if (is_first) {
    wanted_port = port_override().value_or(wanted_port);
  }

  const auto started = server.start(wanted_port, std::move(token));
  if (!started) {
    server.last_error = started.error();
    OX_LOG_ERROR("MCP server failed to start: {}", started.error());
    return;
  }

  OX_LOG_INFO("MCP server listening on http://127.0.0.1:{}/mcp", started.value());
}

static auto sync_discovery(McpServer& server, McpServer::State& state, Editor& editor) -> void {
  std::filesystem::path wanted;
  if (server.is_running && editor.active_project) {
    const auto& directory = editor.active_project->get_project_directory();
    if (!directory.empty()) {
      wanted = directory / DISCOVERY_DIR / DISCOVERY_FILE;
    }
  }

  if (wanted == state.discovery_path && (wanted.empty() || state.discovery_port == server.bound_port)) {
    return;
  }

  if (wanted != state.discovery_path) {
    remove_discovery(state);
  }

  if (wanted.empty()) {
    return;
  }

  write_discovery(wanted, server.bound_port, !state.token.empty());
  state.discovery_path = std::move(wanted);
  state.discovery_port = server.bound_port;
}

auto McpServer::pump(this McpServer& self, Editor& editor) -> void {
  ZoneScoped;

  apply_settings(self, *self.state, editor);
  if (!self.is_running) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  std::erase_if(self.state->pending, [&](McpPendingCall& call) {
    auto polled = call.poll();
    if (polled.has_value()) {
      fulfil(call, polled.value());
      return true;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - call.started).count();
    if (elapsed >= call.timeout_ms) {
      const auto message = fmt::format("timed out after {} ms, the work is still running", call.timeout_ms);
      fulfil(call, McpResult::error(message));
      return true;
    }

    return false;
  });

  std::vector<McpPendingCall> incoming;
  {
    std::unique_lock lock(self.state->mutex);
    incoming.swap(self.state->queue);
  }

  for (auto& call : incoming) {
    if (run_call(self, editor, call)) {
      self.state->pending.emplace_back(std::move(call));
    }
  }

  sync_discovery(self, *self.state, editor);
}
} // namespace ox
