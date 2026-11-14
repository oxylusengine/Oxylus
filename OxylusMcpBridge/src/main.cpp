#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <iostream>
#include <optional>
#include <simdjson.h>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
  #include <process.h>
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#endif

namespace ox {
constexpr static std::string_view BRIDGE_VERSION = "0.1.0";
constexpr static std::string_view DEFAULT_URL = "http://127.0.0.1:7780/mcp";
constexpr static std::string_view TOKEN_ENV = "OXYLUS_MCP_TOKEN";
constexpr static int CONNECT_TIMEOUT_SECONDS = 5;
constexpr static int READ_TIMEOUT_SECONDS = 15 * 60;
constexpr static int WRITE_TIMEOUT_SECONDS = 60;
constexpr static int LAUNCH_WAIT_SECONDS = 180;
constexpr static int ERR_PARSE = -32700;
constexpr static int ERR_UNREACHABLE = -32000;

constexpr static std::string_view HELP_TEXT = R"(oxmcp, stdio bridge to the Oxylus editor's MCP server

usage: oxmcp [options]

Reads newline-delimited JSON-RPC from stdin, forwards each message to the editor's
HTTP endpoint and writes the replies to stdout. Logs go to stderr.

options:
  --url <url>        full endpoint, for example http://127.0.0.1:7780/mcp
  --port <n>         endpoint on 127.0.0.1:<n>
  --project <path>   project directory or .oxproj, the endpoint is read from
                     <project dir>/.oxeditor/mcp.json
                     (without any of the three: http://127.0.0.1:7780/mcp)
  --token <t>        bearer token, or set OXYLUS_MCP_TOKEN
  --launch           when the endpoint is unreachable at startup, start OxylusEditor
                     (next to oxmcp) on --project and wait up to 180 s for it
  --selftest         talk to the editor, print PASS/FAIL/SKIP per step and exit,
                     nonzero if any step failed. Edits the open scene, see undo history
  --version          print the version
  --help             print this text

Claude Code over HTTP:
  claude mcp add --transport http oxylus http://127.0.0.1:7780/mcp

Claude Code over stdio:
  claude mcp add oxylus -- <path>/oxmcp --project .

Any stdio client:
  {"mcpServers":{"oxylus":{"command":"<path>/oxmcp","args":["--project","."]}}}
)";

struct Options {
  std::string url = {};
  int port = 0;
  std::string project = {};
  std::string token = {};
  bool is_launch = false;
  bool is_selftest = false;
  bool is_help = false;
  bool is_version = false;
};

struct Endpoint {
  std::string host = {};
  int port = 0;
  std::string path = {};
  std::string url = {};
};

struct Reply {
  bool is_connected = false;
  int status = 0;
  std::string body = {};
  std::string error = {};
};

static auto log_line(std::string_view text) -> void { std::cerr << "oxmcp: " << text << std::endl; }

static auto json_quote(std::string_view text) -> std::string {
  constexpr std::string_view hex = "0123456789abcdef";

  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"' : out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "\\u00";
          out += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
          out += hex[static_cast<unsigned char>(c) & 0xf];
        } else {
          out += c;
        }
        break;
    }
  }

  out += '"';
  return out;
}

static auto parse_int(std::string_view text) -> std::optional<int> {
  if (text.empty() || text.size() > 9 || !std::ranges::all_of(text, [](char c) -> bool {
        return c >= '0' && c <= '9';
      })) {
    return std::nullopt;
  }

  return std::atoi(std::string(text).c_str());
}

static auto parse_url(std::string_view url) -> std::optional<Endpoint> {
  constexpr std::string_view scheme = "http://";
  if (!url.starts_with(scheme)) {
    return std::nullopt;
  }

  auto rest = url.substr(scheme.size());
  const auto slash = rest.find('/');
  const auto authority = rest.substr(0, slash);
  const std::string path = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));

  Endpoint endpoint = {.host = std::string(authority), .port = 80, .path = path, .url = std::string(url)};
  const auto colon = authority.rfind(':');
  if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
    const auto port = parse_int(authority.substr(colon + 1));
    if (!port.has_value() || *port == 0 || *port > 65535) {
      return std::nullopt;
    }

    endpoint.host = std::string(authority.substr(0, colon));
    endpoint.port = *port;
  }

  if (endpoint.host.empty()) {
    return std::nullopt;
  }

  return endpoint;
}

static auto project_directory(const std::string& project) -> std::filesystem::path {
  std::error_code error;
  const auto path = std::filesystem::path(project);
  if (std::filesystem::is_directory(path, error)) {
    return path;
  }

  return path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
}

static auto find_project_file(const std::string& project) -> std::optional<std::filesystem::path> {
  std::error_code error;
  const auto path = std::filesystem::path(project);
  if (path.extension() == ".oxproj") {
    return path;
  }

  if (!std::filesystem::is_directory(path, error)) {
    return std::nullopt;
  }

  for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
    if (entry.is_regular_file(error) && entry.path().extension() == ".oxproj") {
      return entry.path();
    }
  }

  return std::nullopt;
}

static auto read_discovery_url(const std::string& project) -> std::optional<std::string> {
  const auto file_path = project_directory(project) / ".oxeditor" / "mcp.json";
  std::ifstream file(file_path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  const auto text = buffer.str();

  simdjson::dom::parser parser;
  simdjson::dom::element root;
  std::string_view url;
  if (parser.parse(text.data(), text.size()).get(root) || root["url"].get(url)) {
    return std::nullopt;
  }

  return std::string(url);
}

static auto resolve_endpoint(const Options& options) -> std::optional<Endpoint> {
  if (!options.url.empty()) {
    return parse_url(options.url);
  }

  if (options.port != 0) {
    return parse_url("http://127.0.0.1:" + std::to_string(options.port) + "/mcp");
  }

  if (!options.project.empty()) {
    if (const auto discovered = read_discovery_url(options.project); discovered.has_value()) {
      if (const auto endpoint = parse_url(*discovered); endpoint.has_value()) {
        return endpoint;
      }
    }
  }

  return parse_url(DEFAULT_URL);
}

static auto post(
  const Endpoint& endpoint, const Options& options, const std::string& body, const httplib::Headers& extra_headers = {}
) -> Reply {
  httplib::Client client(endpoint.host, endpoint.port);
  client.set_connection_timeout(CONNECT_TIMEOUT_SECONDS, 0);
  client.set_read_timeout(READ_TIMEOUT_SECONDS, 0);
  client.set_write_timeout(WRITE_TIMEOUT_SECONDS, 0);

  httplib::Headers headers = extra_headers;
  headers.emplace("Accept", "application/json, text/event-stream");
  if (!options.token.empty()) {
    headers.emplace("Authorization", "Bearer " + options.token);
  }

  const auto result = client.Post(endpoint.path, headers, body, "application/json");
  if (!result) {
    return Reply{.error = httplib::to_string(result.error())};
  }

  return Reply{.is_connected = true, .status = result->status, .body = result->body};
}

static auto post_with_retry(Endpoint& endpoint, const Options& options, const std::string& body) -> Reply {
  auto reply = post(endpoint, options, body);
  if (reply.is_connected) {
    return reply;
  }

  const auto resolved = resolve_endpoint(options);
  if (resolved.has_value()) {
    endpoint = *resolved;
  }

  return post(endpoint, options, body);
}

static auto is_reachable(const Endpoint& endpoint, const Options& options) -> bool {
  const auto reply = post(endpoint, options, R"({"jsonrpc":"2.0","id":0,"method":"ping"})");
  return reply.is_connected && reply.status == 200;
}

static auto write_stdout_line(std::string line) -> void {
  std::ranges::replace(line, '\n', ' ');
  std::ranges::replace(line, '\r', ' ');
  while (!line.empty() && line.back() == ' ') {
    line.pop_back();
  }

  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fflush(stdout);
}

static auto error_line(std::string_view id_json, int code, std::string_view message) -> std::string {
  return R"({"jsonrpc":"2.0","id":)" + std::string(id_json) + R"(,"error":{"code":)" + std::to_string(code) +
         R"(,"message":)" + json_quote(message) + "}}";
}

static auto unreachable_message(const Endpoint& endpoint) -> std::string {
  return "the Oxylus editor is not running or not reachable at " + endpoint.url +
         ", start it (oxmcp --launch) and retry";
}

static auto self_directory(const char* argv0) -> std::filesystem::path {
  std::error_code error;
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length > 0) {
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    return std::filesystem::weakly_canonical(buffer.c_str(), error).parent_path();
  }
#else
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error) {
    return exe.parent_path();
  }
#endif
  return std::filesystem::absolute(argv0, error).parent_path();
}

static auto spawn_detached(
  const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::filesystem::path& cwd
) -> bool {
#ifdef _WIN32
  std::wstring command = L"\"" + executable.wstring() + L"\"";
  for (const auto& argument : arguments) {
    command += L" \"" + std::filesystem::path(argument).wstring() + L"\"";
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  const auto is_started = CreateProcessW(
    executable.c_str(),
    command.data(),
    nullptr,
    nullptr,
    FALSE,
    DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
    nullptr,
    cwd.c_str(),
    &startup,
    &process
  );
  if (!is_started) {
    return false;
  }

  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return true;
#else
  const auto executable_text = executable.string();
  const auto cwd_text = cwd.string();
  std::vector<char*> argv;
  std::vector<std::string> storage = {executable_text};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  for (auto& text : storage) {
    argv.push_back(text.data());
  }

  argv.push_back(nullptr);

  const auto pid = fork();
  if (pid < 0) {
    return false;
  }

  if (pid == 0) {
    setsid();
    if (chdir(cwd_text.c_str()) != 0) {
      _exit(127);
    }

    const auto null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }

    execv(executable_text.c_str(), argv.data());
    _exit(127);
  }

  return true;
#endif
}

static auto launch_editor(const Options& options, const char* argv0, Endpoint& endpoint) -> bool {
  const auto directory = self_directory(argv0);
#ifdef _WIN32
  const auto editor = directory / "OxylusEditor.exe";
#else
  const auto editor = directory / "OxylusEditor";
#endif

  std::error_code error;
  if (!std::filesystem::is_regular_file(editor, error)) {
    log_line("cannot launch, " + editor.string() + " does not exist");
    return false;
  }

  std::vector<std::string> arguments;
  if (!options.project.empty()) {
    const auto project_file = find_project_file(options.project);
    if (project_file.has_value()) {
      arguments.emplace_back("--project");
      arguments.emplace_back(std::filesystem::absolute(*project_file, error).string());
    } else {
      log_line("no .oxproj found at " + options.project + ", starting the editor without a project");
    }
  }

  if (options.port != 0) {
    arguments.emplace_back("--mcp-port");
    arguments.emplace_back(std::to_string(options.port));
  }

  log_line("starting " + editor.string());
  if (!spawn_detached(editor, arguments, directory)) {
    log_line("could not start the editor");
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(LAUNCH_WAIT_SECONDS);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto resolved = resolve_endpoint(options);
    if (resolved.has_value() && is_reachable(*resolved, options)) {
      endpoint = *resolved;
      log_line("the editor is listening at " + endpoint.url);
      return true;
    }
  }

  log_line("the editor did not start listening within " + std::to_string(LAUNCH_WAIT_SECONDS) + " s");
  return false;
}

static auto handle_line(const std::string& line, Endpoint& endpoint, const Options& options) -> void {
  simdjson::dom::parser parser;
  simdjson::dom::element root;
  if (parser.parse(line.data(), line.size()).get(root)) {
    write_stdout_line(error_line("null", ERR_PARSE, "parse error"));
    return;
  }

  simdjson::dom::object object;
  simdjson::dom::element id_element;
  simdjson::dom::element method_element;
  const auto is_request = !root.get(object) && !object["id"].get(id_element) && !object["method"].get(method_element);

  const auto id_json = is_request ? simdjson::minify(id_element) : std::string("null");

  const auto reply = post_with_retry(endpoint, options, line);
  if (!reply.is_connected) {
    if (is_request) {
      write_stdout_line(error_line(id_json, ERR_UNREACHABLE, unreachable_message(endpoint)));
    }

    return;
  }

  const auto first = reply.body.find_first_not_of(" \t\r\n");
  if (reply.status == 202 || first == std::string::npos) {
    return;
  }

  if (reply.body[first] == '{' || reply.body[first] == '[') {
    write_stdout_line(reply.body);
    return;
  }

  if (is_request) {
    write_stdout_line(error_line(
      id_json,
      ERR_UNREACHABLE,
      "the editor answered HTTP " + std::to_string(reply.status) + " at " + endpoint.url
    ));
  }
}

static auto serve(Endpoint& endpoint, const Options& options) -> int {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.find_first_not_of(" \t") == std::string::npos) {
      continue;
    }

    handle_line(line, endpoint, options);
  }

  return 0;
}

struct ToolInfo {
  std::string name = {};
  bool is_read_only = false;
  bool has_required = false;
  bool has_timeout = false;
};

struct ToolResult {
  bool is_ok = false;
  bool is_error = false;
  std::string text = {};
  std::string reason = {};
};

struct SelfTest {
  Endpoint endpoint = {};
  Options options = {};
  int next_id = 1;
  int pass_count = 0;
  int fail_count = 0;
  bool is_scene_open = false;

  auto pass(this SelfTest& self, std::string_view name) -> void {
    self.pass_count += 1;
    std::cout << "PASS " << name << std::endl;
  }

  auto fail(this SelfTest& self, std::string_view name, std::string_view reason) -> void {
    self.fail_count += 1;
    std::cout << "FAIL " << name << ": " << reason << std::endl;
  }

  auto skip(this SelfTest& self, std::string_view name, std::string_view reason) -> void {
    std::cout << "SKIP " << name << ": " << reason << std::endl;
  }

  auto check(this SelfTest& self, std::string_view name, bool is_ok, std::string_view reason) -> bool {
    if (is_ok) {
      self.pass(name);
    } else {
      self.fail(name, reason);
    }

    return is_ok;
  }

  auto rpc(this SelfTest& self, std::string_view method, std::string_view params, const httplib::Headers& headers = {})
    -> Reply {
    const auto body = R"({"jsonrpc":"2.0","id":)" + std::to_string(self.next_id++) + R"(,"method":)" +
                      json_quote(method) + R"(,"params":)" + std::string(params) + "}";
    return post_with_retry(self.endpoint, self.options, body);
  }

  auto call_tool(this SelfTest& self, std::string_view name, std::string_view arguments) -> ToolResult {
    const auto params = R"({"name":)" + json_quote(name) + R"(,"arguments":)" + std::string(arguments) + "}";
    const auto reply = self.rpc("tools/call", params);
    if (!reply.is_connected) {
      return ToolResult{.reason = "the editor is not reachable at " + self.endpoint.url};
    }

    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(reply.body.data(), reply.body.size()).get(root)) {
      return ToolResult{.reason = "HTTP " + std::to_string(reply.status) + ", the body is not JSON"};
    }

    simdjson::dom::element error;
    if (!root["error"].get(error)) {
      std::string_view message;
      std::ignore = error["message"].get(message);
      return ToolResult{.reason = "json-rpc error: " + std::string(message)};
    }

    ToolResult result = {.is_ok = true};
    std::string_view text;
    if (!root["result"]["content"].at(0)["text"].get(text)) {
      result.text = std::string(text);
    }

    bool is_error = false;
    if (!root["result"]["isError"].get(is_error)) {
      result.is_error = is_error;
    }

    return result;
  }

  auto call_ok(this SelfTest& self, std::string_view step, std::string_view name, std::string_view arguments)
    -> ToolResult {
    auto result = self.call_tool(name, arguments);
    if (!result.is_ok) {
      self.fail(step, result.reason);
      result.is_ok = false;
    } else if (result.is_error) {
      self.fail(step, result.text);
      result.is_ok = false;
    } else {
      self.pass(step);
    }

    return result;
  }

  auto read_position(this SelfTest& self, const std::string& ref) -> std::optional<std::array<double, 3>> {
    const auto result = self.call_tool(
      "entity_get",
      R"({"entities":[)" + json_quote(ref) + R"(],"components":["TransformComponent"]})"
    );
    if (!result.is_ok || result.is_error) {
      return std::nullopt;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(result.text.data(), result.text.size()).get(root)) {
      return std::nullopt;
    }

    std::array<double, 3> position = {};
    constexpr std::array<std::string_view, 3> axes = {
      "/entities/0/components/TransformComponent/position/x",
      "/entities/0/components/TransformComponent/position/y",
      "/entities/0/components/TransformComponent/position/z",
    };
    for (std::size_t i = 0; i < axes.size(); i++) {
      if (root.at_pointer(axes[i]).get(position[i])) {
        return std::nullopt;
      }
    }

    return position;
  }

  auto check_position(this SelfTest& self, std::string_view step, const std::string& ref, std::array<double, 3> wanted)
    -> void {
    const auto position = self.read_position(ref);
    if (!position.has_value()) {
      self.fail(step, "could not read TransformComponent.position of " + ref);
      return;
    }

    const auto is_match = std::abs((*position)[0] - wanted[0]) < 1e-4 && std::abs((*position)[1] - wanted[1]) < 1e-4 &&
                          std::abs((*position)[2] - wanted[2]) < 1e-4;
    char got[96];
    std::snprintf(got, sizeof(got), "position is [%g, %g, %g]", (*position)[0], (*position)[1], (*position)[2]);
    self.check(step, is_match, got);
  }

  auto run_protocol(this SelfTest& self) -> bool {
    const auto initialize = self.rpc(
      "initialize",
      R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"oxmcp-selftest","version":")" +
        std::string(BRIDGE_VERSION) + R"("}})"
    );
    if (!initialize.is_connected) {
      self.fail("initialize", "the editor is not reachable at " + self.endpoint.url + " (" + initialize.error + ")");
      return false;
    }

    simdjson::dom::parser parser;
    simdjson::dom::element root;
    std::string_view negotiated;
    const auto is_parsed = !parser.parse(initialize.body.data(), initialize.body.size()).get(root);
    const auto is_initialized = is_parsed && !root["result"]["protocolVersion"].get(negotiated) &&
                                negotiated == "2025-06-18" &&
                                root["result"]["serverInfo"]["name"].error() == simdjson::SUCCESS;
    if (!self.check(
          "initialize",
          is_initialized,
          "HTTP " + std::to_string(initialize.status) + ": " + initialize.body.substr(0, 200)
        )) {
      return false;
    }

    const auto initialized = post_with_retry(
      self.endpoint,
      self.options,
      R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
    );
    self.check("notifications/initialized", initialized.is_connected && initialized.status == 202, "expected HTTP 202");
    return true;
  }

  auto run_tools_list(this SelfTest& self, std::vector<ToolInfo>& tools) -> void {
    const auto reply = self.rpc("tools/list", "{}");
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    simdjson::dom::array list;
    if (
      !reply.is_connected || parser.parse(reply.body.data(), reply.body.size()).get(root) ||
      root["result"]["tools"].get(list)
    ) {
      self.fail("tools/list", "no tools array in the reply");
      return;
    }

    std::string problem;
    for (const auto tool : list) {
      std::string_view name;
      simdjson::dom::object schema;
      if (tool["name"].get(name) || tool["inputSchema"].get(schema)) {
        problem = "a tool is missing name or inputSchema";
        continue;
      }

      ToolInfo info = {.name = std::string(name)};
      bool is_read_only = false;
      if (!tool["annotations"]["readOnlyHint"].get(is_read_only)) {
        info.is_read_only = is_read_only;
      }

      simdjson::dom::array required;
      info.has_required = !schema["required"].get(required) && required.size() > 0;
      info.has_timeout = schema["properties"]["timeout_ms"].error() == simdjson::SUCCESS;
      tools.push_back(std::move(info));
    }

    if (tools.empty()) {
      problem = "the tool list is empty";
    }

    self.check("tools/list", problem.empty(), problem);
  }

  auto run_discover(this SelfTest& self) -> void {
    const httplib::Headers headers = {{"MCP-Protocol-Version", "2026-07-28"}};
    const auto reply = self.rpc(
      "server/discover",
      R"({"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}})",
      headers
    );

    simdjson::dom::parser parser;
    simdjson::dom::element root;
    simdjson::dom::array versions;
    const auto is_ok = reply.is_connected && !parser.parse(reply.body.data(), reply.body.size()).get(root) &&
                       !root["result"]["supportedVersions"].get(versions) && versions.size() > 0;
    self.check("server/discover", is_ok, "no supportedVersions in the reply, HTTP " + std::to_string(reply.status));
  }

  auto run_read_only_tools(this SelfTest& self, std::span<const ToolInfo> tools) -> void {
    for (const auto& tool : tools) {
      if (!tool.is_read_only || tool.has_required || tool.name == "editor_state") {
        continue;
      }

      const auto step = "tool " + tool.name;
      const auto result = self.call_tool(tool.name, tool.has_timeout ? R"({"timeout_ms":1000})" : "{}");
      if (!result.is_ok) {
        self.fail(step, result.reason);
      } else if (result.is_error && !self.is_scene_open && result.text.contains("scene")) {
        self.skip(step, "needs an open scene");
      } else if (result.is_error) {
        self.fail(step, result.text.substr(0, 300));
      } else {
        self.pass(step);
      }
    }
  }

  auto run_state(this SelfTest& self) -> void {
    const auto state = self.call_tool("editor_state", "{}");
    self.is_scene_open = state.is_ok && !state.is_error && state.text.contains("scenes: ") &&
                         !state.text.contains("scenes: 0") && state.text.contains(" active");
    if (!state.is_ok || state.is_error) {
      self.fail("editor_state", state.is_ok ? state.text : state.reason);
      return;
    }

    self.pass("editor_state");
  }

  auto run_mutations(this SelfTest& self) -> void {
    const std::array<std::string_view, 11> steps = {
      "entity_create",
      "entity_update",
      "entity_get after update",
      "editor_undo",
      "entity_get after undo",
      "editor_redo",
      "entity_get after redo",
      "entity_delete",
      "editor_undo of delete",
      "entity_get after undo of delete",
      "entity_delete cleanup",
    };
    if (!self.is_scene_open) {
      for (const auto step : steps) {
        self.skip(step, "no scene is open");
      }

      return;
    }

    const auto name = "mcp_selftest_" + std::to_string(
#ifdef _WIN32
                                          _getpid()
#else
                                          getpid()
#endif
                                        );
    const auto created = self.call_ok(steps[0], "entity_create", R"({"entities":[{"name":)" + json_quote(name) + "}]}");
    if (!created.is_ok) {
      return;
    }

    const auto hash = created.text.find('#');
    const auto path_start = hash == std::string::npos ? std::string::npos : created.text.find(' ', hash);
    if (path_start == std::string::npos) {
      self.fail(steps[1], "could not read the created entity from: " + created.text.substr(0, 200));
      return;
    }

    auto ref = created.text.substr(path_start + 1);
    ref = ref.substr(0, ref.find_first_of("\r\n"));
    const auto entities = R"({"entities":[)" + json_quote(ref) + "]}";

    bool is_deleted = false;
    const auto update = self.call_ok(
      steps[1],
      "entity_update",
      R"({"entities":[{"entity":)" + json_quote(ref) + R"(,"set":{"TransformComponent":{"position":[1,2,3]}}}]})"
    );
    if (update.is_ok) {
      self.check_position(steps[2], ref, {1.0, 2.0, 3.0});
      if (self.call_ok(steps[3], "editor_undo", "{}").is_ok) {
        self.check_position(steps[4], ref, {0.0, 0.0, 0.0});
      }

      if (self.call_ok(steps[5], "editor_redo", "{}").is_ok) {
        self.check_position(steps[6], ref, {1.0, 2.0, 3.0});
      }

      if (self.call_ok(steps[7], "entity_delete", entities).is_ok) {
        is_deleted = true;
        if (self.call_ok(steps[8], "editor_undo", "{}").is_ok) {
          is_deleted = false;
          self.check_position(steps[9], ref, {1.0, 2.0, 3.0});
        }
      }
    }

    if (is_deleted) {
      self.skip(steps[10], "already deleted");
    } else {
      self.call_ok(steps[10], "entity_delete", entities);
    }

    std::cout << "note: the undo history now has the selftest steps" << std::endl;
  }

  auto run(this SelfTest& self) -> int {
    if (!self.run_protocol()) {
      return 1;
    }

    std::vector<ToolInfo> tools;
    self.run_tools_list(tools);
    self.run_discover();
    self.run_state();
    self.run_read_only_tools(tools);
    self.run_mutations();

    std::cout << self.pass_count << " passed, " << self.fail_count << " failed" << std::endl;
    return self.fail_count == 0 ? 0 : 1;
  }
};

static auto parse_options(int argc, char** argv, Options& options) -> bool {
  for (int i = 1; i < argc; i++) {
    const std::string_view flag = argv[i];
    const auto is_flag_with_value = flag == "--url" || flag == "--port" || flag == "--project" || flag == "--token";
    if (is_flag_with_value && i + 1 >= argc) {
      log_line("missing value for " + std::string(flag));
      return false;
    }

    if (flag == "--url") {
      options.url = argv[++i];
    } else if (flag == "--port") {
      const auto port = parse_int(argv[++i]);
      if (!port.has_value() || *port == 0 || *port > 65535) {
        log_line("--port needs a number from 1 to 65535");
        return false;
      }

      options.port = *port;
    } else if (flag == "--project") {
      options.project = argv[++i];
    } else if (flag == "--token") {
      options.token = argv[++i];
    } else if (flag == "--launch") {
      options.is_launch = true;
    } else if (flag == "--selftest") {
      options.is_selftest = true;
    } else if (flag == "--help" || flag == "-h") {
      options.is_help = true;
    } else if (flag == "--version") {
      options.is_version = true;
    } else {
      log_line("unknown option " + std::string(flag) + ", see --help");
      return false;
    }
  }

  if (options.token.empty()) {
    if (const auto* env_token = std::getenv(TOKEN_ENV.data()); env_token != nullptr) {
      options.token = env_token;
    }
  }

  return true;
}

static auto run(int argc, char** argv) -> int {
  Options options;
  if (!parse_options(argc, argv, options)) {
    return 2;
  }

  if (options.is_help) {
    std::cout << HELP_TEXT;
    return 0;
  }

  if (options.is_version) {
    std::cout << "oxmcp " << BRIDGE_VERSION << std::endl;
    return 0;
  }

  auto endpoint = resolve_endpoint(options);
  if (!endpoint.has_value()) {
    log_line("the endpoint is not a valid http://host:port/path url");
    return 2;
  }

  if (options.is_selftest) {
    SelfTest test = {.endpoint = *endpoint, .options = options};
    return test.run();
  }

#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  if (options.is_launch && !is_reachable(*endpoint, options)) {
    if (!launch_editor(options, argv[0], *endpoint)) {
      log_line("continuing without the editor, requests will answer with an error");
    }
  }

  return serve(*endpoint, options);
}
} // namespace ox

int main(int argc, char** argv) { return ox::run(argc, argv); }
