#include "Mcp/McpProtocol.hpp"

#include <array>
#include <fmt/format.h>
#include <simdjson.h>
#include <simdutf.h>
#include <tracy/Tracy.hpp>

#include "Utils/JsonWriter.hpp"
#include "Utils/StringUtils.hpp"

namespace ox {
constexpr static std::string_view MODERN_VERSION = "2026-07-28";
constexpr static std::array<std::string_view, 4> LEGACY_VERSIONS = {
  "2024-11-05",
  "2025-03-26",
  "2025-06-18",
  "2025-11-25",
};
constexpr static std::string_view META_VERSION_KEY = "io.modelcontextprotocol/protocolVersion";
constexpr static std::string_view META_CAPABILITIES_KEY = "io.modelcontextprotocol/clientCapabilities";
constexpr static std::string_view META_SERVER_INFO_KEY = "io.modelcontextprotocol/serverInfo";
constexpr static std::string_view SERVER_NAME = "oxylus-editor";
constexpr static std::string_view SERVER_TITLE = "Oxylus Editor";
constexpr static std::string_view
  INSTRUCTIONS = "Controls a running Oxylus Editor.\n"
                 "Call editor_state first to orient: project, open scenes, selection, panels, jobs.\n"
                 "Address entities by path like \"Level/Props/Crate\" or by id like \"#1234\".\n"
                 "Use the file_* tools to move and rename project files so sidecars and asset links follow.\n"
                 "Read editor_guide before a multi-step job.\n"
                 "Every scene edit is undoable from the editor.";

constexpr static i32 ERR_PARSE = -32700;
constexpr static i32 ERR_INVALID_REQUEST = -32600;
constexpr static i32 ERR_METHOD_NOT_FOUND = -32601;
constexpr static i32 ERR_INVALID_PARAMS = -32602;
constexpr static i32 ERR_HEADER_MISMATCH = -32020;
constexpr static i32 ERR_UNSUPPORTED_VERSION = -32022;

static auto is_legacy_version(std::string_view version) -> bool {
  for (const auto legacy : LEGACY_VERSIONS) {
    if (legacy == version) {
      return true;
    }
  }

  return false;
}

static auto begin_response(JsonWriter& writer, std::string_view id) -> void {
  writer.is_pretty = false;
  writer.begin_obj();
  writer.key("jsonrpc");
  writer.str("2.0");
  writer.key("id");
  writer.raw(id);
}

static auto error_reply(
  u32 status, std::string_view id, i32 code, std::string_view message, std::string_view data_json = {}
) -> McpReply {
  JsonWriter writer;
  begin_response(writer, id);
  writer.key("error");
  writer.begin_obj();
  writer["code"] = code;
  writer.key("message");
  writer.str(message);
  if (!data_json.empty()) {
    writer.key("data");
    writer.raw(data_json);
  }
  writer.end_obj();
  writer.end_obj();

  return McpReply{.status = status, .body = writer.stream.str()};
}

template <typename Fn>
static auto result_reply(std::string_view id, bool is_modern, Fn&& write_fields) -> McpReply {
  JsonWriter writer;
  begin_response(writer, id);
  writer.key("result");
  writer.begin_obj();
  if (is_modern) {
    writer.key("resultType");
    writer.str("complete");
  }
  write_fields(writer);
  writer.end_obj();
  writer.end_obj();

  return McpReply{.status = 200, .body = writer.stream.str()};
}

static auto id_to_json(simdjson::dom::element id) -> option<std::string> {
  switch (id.type()) {
    case simdjson::dom::element_type::INT64: {
      i64 value = 0;
      std::ignore = id.get(value);
      return std::to_string(value);
    }
    case simdjson::dom::element_type::UINT64: {
      u64 value = 0;
      std::ignore = id.get(value);
      return std::to_string(value);
    }
    case simdjson::dom::element_type::DOUBLE: {
      f64 value = 0.0;
      std::ignore = id.get(value);
      return fmt::format("{}", value);
    }
    case simdjson::dom::element_type::STRING: {
      std::string_view value;
      std::ignore = id.get(value);
      return "\"" + StringUtils::escape_str(value) + "\"";
    }
    case simdjson::dom::element_type::NULL_VALUE: return std::string("null");
    default                                     : return nullopt;
  }
}

static auto write_tool(JsonWriter& writer, const McpTool& tool) -> void {
  writer.begin_obj();
  writer.key("name");
  writer.str(tool.name);
  writer.key("title");
  writer.str(tool.title);
  writer.key("description");
  writer.str(tool.description);
  writer.key("inputSchema");
  writer.raw(tool.input_schema.empty() ? R"({"type":"object","properties":{}})" : tool.input_schema);
  writer.key("annotations");
  writer.begin_obj();
  writer["readOnlyHint"] = tool.is_read_only;
  writer["destructiveHint"] = tool.is_destructive;
  writer["idempotentHint"] = tool.is_idempotent;
  writer["openWorldHint"] = false;
  writer.end_obj();
  writer.end_obj();
}

static auto join_names(std::span<const std::string> names) -> std::string {
  std::string out;
  for (usize i = 0; i < names.size(); i++) {
    if (i > 0) {
      out += ", ";
    }

    out += names[i];
  }

  return out;
}

static auto handle_tools_call(
  simdjson::dom::object params, std::string_view id, bool is_modern, std::span<const McpToolEntry> tools
) -> McpReply {
  std::string_view tool_name;
  if (params.at_key("name").get(tool_name)) {
    return error_reply(200, id, ERR_INVALID_PARAMS, "params.name: expected string");
  }

  usize tool_index = tools.size();
  for (usize i = 0; i < tools.size(); i++) {
    if (tools[i].tool.name == tool_name) {
      tool_index = i;
      break;
    }
  }

  if (tool_index == tools.size()) {
    return error_reply(200, id, ERR_INVALID_PARAMS, fmt::format("unknown tool '{}'", tool_name));
  }

  McpCallRequest call = {.tool_index = tool_index, .is_modern = is_modern, .id = std::string(id)};

  simdjson::dom::element arguments;
  if (!params.at_key("arguments").get(arguments) && arguments.type() != simdjson::dom::element_type::NULL_VALUE) {
    simdjson::dom::object arguments_object;
    if (arguments.get(arguments_object)) {
      return error_reply(200, id, ERR_INVALID_PARAMS, "params.arguments: expected object");
    }

    const auto& arg_names = tools[tool_index].arg_names;
    for (const auto& field : arguments_object) {
      bool is_known = false;
      for (const auto& name : arg_names) {
        is_known = is_known || name == field.key;
      }

      if (!is_known) {
        const auto valid = arg_names.empty() ? std::string("this tool takes no arguments")
                                             : "valid arguments: " + join_names(arg_names);
        return error_reply(200, id, ERR_INVALID_PARAMS, fmt::format("unknown argument '{}', {}", field.key, valid));
      }
    }

    call.arguments = simdjson::minify(arguments);
  }

  return McpReply{.status = 200, .body = {}, .call = std::move(call)};
}

auto mcp_handle_post(
  std::string_view body,
  option<std::string_view> version_header,
  std::span<const McpToolEntry> tools,
  std::string_view version
) -> McpReply {
  ZoneScoped;

  const auto server_version = version.empty() ? std::string_view("0.1.0") : version;

  simdjson::dom::parser parser;
  simdjson::dom::element root;
  if (parser.parse(body.data(), body.size()).get(root)) {
    return error_reply(400, "null", ERR_PARSE, "parse error");
  }

  if (root.type() == simdjson::dom::element_type::ARRAY) {
    return error_reply(400, "null", ERR_INVALID_REQUEST, "batching is not supported");
  }

  simdjson::dom::object request;
  if (root.get(request)) {
    return error_reply(400, "null", ERR_INVALID_REQUEST, "request must be an object");
  }

  std::string id = "null";
  bool has_id = false;
  simdjson::dom::element id_element;
  if (!request.at_key("id").get(id_element)) {
    has_id = true;
    auto parsed_id = id_to_json(id_element);
    if (!parsed_id.has_value()) {
      return error_reply(400, "null", ERR_INVALID_REQUEST, "id must be a string, number or null");
    }

    id = std::move(parsed_id.value());
  }

  std::string_view method;
  simdjson::dom::element method_element;
  if (request.at_key("method").get(method_element)) {
    simdjson::dom::element ignored;
    const auto is_response = !request.at_key("result").get(ignored) || !request.at_key("error").get(ignored);
    if (is_response) {
      return McpReply{.status = 202};
    }

    return error_reply(400, id, ERR_INVALID_REQUEST, "missing method");
  }

  if (method_element.get(method)) {
    return error_reply(400, id, ERR_INVALID_REQUEST, "method must be a string");
  }

  if (!has_id) {
    return McpReply{.status = 202};
  }

  simdjson::dom::object params;
  bool has_params = false;
  simdjson::dom::element params_element;
  if (
    !request.at_key("params").get(params_element) && params_element.type() != simdjson::dom::element_type::NULL_VALUE
  ) {
    if (params_element.get(params)) {
      return error_reply(400, id, ERR_INVALID_PARAMS, "params must be an object");
    }

    has_params = true;
  }

  bool is_modern = false;
  std::string_view meta_version;
  bool has_capabilities = false;
  if (has_params && method != "initialize") {
    simdjson::dom::object meta;
    simdjson::dom::element version_element;
    simdjson::dom::element capabilities_element;
    if (!params.at_key("_meta").get(meta) && !meta.at_key(META_VERSION_KEY).get(version_element)) {
      is_modern = true;
      std::ignore = version_element.get(meta_version);
      has_capabilities = !meta.at_key(META_CAPABILITIES_KEY).get(capabilities_element);
    }
  }

  if (is_modern) {
    if (!has_capabilities) {
      return error_reply(400, id, ERR_INVALID_PARAMS, "missing _meta clientCapabilities");
    }

    if (meta_version != MODERN_VERSION) {
      const auto data = fmt::format(
        R"({{"supported":["{}"],"requested":"{}"}})",
        MODERN_VERSION,
        StringUtils::escape_str(meta_version)
      );
      return error_reply(400, id, ERR_UNSUPPORTED_VERSION, "unsupported protocol version", data);
    }

    if (version_header.has_value() && version_header.value() != meta_version) {
      return error_reply(400, id, ERR_HEADER_MISMATCH, "MCP-Protocol-Version header does not match _meta");
    }
  } else if (
    version_header.has_value() && !is_legacy_version(version_header.value()) && version_header.value() != MODERN_VERSION
  ) {
    const auto data = fmt::format(
      R"({{"supported":["2024-11-05","2025-03-26","2025-06-18","2025-11-25","{}"],"requested":"{}"}})",
      MODERN_VERSION,
      StringUtils::escape_str(version_header.value())
    );
    return error_reply(400, id, ERR_UNSUPPORTED_VERSION, "unsupported protocol version", data);
  }

  if (method == "initialize") {
    std::string_view client_version;
    std::string_view negotiated = LEGACY_VERSIONS.back();
    if (has_params && !params.at_key("protocolVersion").get(client_version) && is_legacy_version(client_version)) {
      negotiated = client_version;
    }

    return result_reply(id, false, [&](JsonWriter& writer) {
      writer.key("protocolVersion");
      writer.str(negotiated);
      writer.key("capabilities");
      writer.raw(R"({"tools":{"listChanged":false}})");
      writer.key("serverInfo");
      writer.begin_obj();
      writer.key("name");
      writer.str(SERVER_NAME);
      writer.key("title");
      writer.str(SERVER_TITLE);
      writer.key("version");
      writer.str(server_version);
      writer.end_obj();
      writer.key("instructions");
      writer.str(INSTRUCTIONS);
    });
  }

  if (method == "ping") {
    return result_reply(id, is_modern, [](JsonWriter&) {});
  }

  if (method == "server/discover") {
    return result_reply(id, true, [&](JsonWriter& writer) {
      writer.key("supportedVersions");
      writer.raw(fmt::format(R"(["{}"])", MODERN_VERSION));
      writer.key("capabilities");
      writer.raw(R"({"tools":{}})");
      writer.key("instructions");
      writer.str(INSTRUCTIONS);
      writer["ttlMs"] = 60000;
      writer.key("cacheScope");
      writer.str("private");
      writer.key("_meta");
      writer.begin_obj();
      writer.key(META_SERVER_INFO_KEY);
      writer.begin_obj();
      writer.key("name");
      writer.str(SERVER_NAME);
      writer.key("version");
      writer.str(server_version);
      writer.end_obj();
      writer.end_obj();
    });
  }

  if (method == "tools/list") {
    return result_reply(id, is_modern, [&](JsonWriter& writer) {
      writer.key("tools");
      writer.begin_array();
      for (const auto& entry : tools) {
        write_tool(writer, entry.tool);
      }
      writer.end_array();

      if (is_modern) {
        writer["ttlMs"] = 60000;
        writer.key("cacheScope");
        writer.str("private");
      }
    });
  }

  if (method == "tools/call") {
    if (!has_params) {
      return error_reply(200, id, ERR_INVALID_PARAMS, "missing params");
    }

    return handle_tools_call(params, id, is_modern, tools);
  }

  return error_reply(is_modern ? 404 : 200, id, ERR_METHOD_NOT_FOUND, fmt::format("method not found: {}", method));
}

auto mcp_tool_response(const McpCallRequest& call, const McpResult& result) -> std::string {
  ZoneScoped;

  return result_reply(
           call.id,
           call.is_modern,
           [&](JsonWriter& writer) {
             writer.key("content");
             writer.begin_array();

             writer.begin_obj();
             writer.key("type");
             writer.str("text");
             writer.key("text");
             writer.str(result.text);
             writer.end_obj();

             if (!result.image_png.empty()) {
               std::string encoded(simdutf::base64_length_from_binary(result.image_png.size()), '\0');
               simdutf::binary_to_base64(result.image_png.data(), result.image_png.size(), encoded.data());

               writer.begin_obj();
               writer.key("type");
               writer.str("image");
               writer.key("data");
               writer.str(encoded);
               writer.key("mimeType");
               writer.str("image/png");
               writer.end_obj();
             }

             writer.end_array();

             if (result.is_error) {
               writer["isError"] = true;
             }
           }
  ).body;
}
} // namespace ox
