#include "Mcp/McpHttp.hpp"

#include <fmt/format.h>
#include <httplib.h>
#include <thread>
#include <tracy/Tracy.hpp>

#include "Mcp/McpProtocol.hpp"
#include "Mcp/McpServer.hpp"

namespace ox {
constexpr static usize MAX_PAYLOAD_BYTES = 32ull * 1024ull * 1024ull;
constexpr static u16 PORT_ATTEMPTS = 11;
constexpr static u32 WAIT_SLACK_MS = 10000;

struct McpHttp::Impl {
  httplib::Server server = {};
  std::thread thread = {};
};

static auto is_local_origin(std::string_view origin) -> bool {
  const auto scheme_end = origin.find("://");
  if (scheme_end != std::string_view::npos) {
    origin.remove_prefix(scheme_end + 3);
  }

  origin = origin.substr(0, origin.find('/'));

  std::string_view host = origin;
  if (origin.starts_with('[')) {
    const auto close = origin.find(']');
    if (close == std::string_view::npos) {
      return false;
    }

    host = origin.substr(0, close + 1);
  } else {
    host = origin.substr(0, origin.find(':'));
  }

  return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

McpHttp::McpHttp() = default;
McpHttp::~McpHttp() { stop(); }

auto McpHttp::start(this McpHttp& self, McpServer& server, u16 port, std::string token, std::string_view version)
  -> std::expected<u16, std::string> {
  ZoneScoped;

  if (self.impl) {
    self.stop();
  }

  auto impl = std::make_unique<Impl>();
  auto& http = impl->server;

  http.set_payload_max_length(MAX_PAYLOAD_BYTES);

  http.set_pre_routing_handler([bearer = "Bearer " + token](const httplib::Request& req, httplib::Response& res) {
    if (req.has_header("Origin") && !is_local_origin(req.get_header_value("Origin"))) {
      res.status = 403;
      return httplib::Server::HandlerResponse::Handled;
    }

    if (bearer.size() > 7 && req.get_header_value("Authorization") != bearer) {
      res.status = 401;
      res.set_header("WWW-Authenticate", "Bearer");
      return httplib::Server::HandlerResponse::Handled;
    }

    return httplib::Server::HandlerResponse::Unhandled;
  });

  http.Post(
    "/mcp",
    [&server, server_version = std::string(version)](const httplib::Request& req, httplib::Response& res) {
      ZoneScopedN("mcp post");

      const auto header_text = req.get_header_value("MCP-Protocol-Version");
      option<std::string_view> version_header = nullopt;
      if (req.has_header("MCP-Protocol-Version")) {
        version_header = std::string_view(header_text);
      }

      auto reply = mcp_handle_post(req.body, version_header, server.tools, server_version);
      if (reply.call.has_value()) {
        const auto timeout_ms = server.tools[reply.call->tool_index].tool.timeout_ms;
        auto body = server.submit_and_wait(
          std::move(reply.call.value()),
          std::chrono::milliseconds(timeout_ms + WAIT_SLACK_MS)
        );
        res.status = 200;
        res.set_content(std::move(body), "application/json");
        return;
      }

      res.status = static_cast<i32>(reply.status);
      if (!reply.body.empty()) {
        res.set_content(std::move(reply.body), "application/json");
      }
    }
  );

  const auto reject_method = [](const httplib::Request&, httplib::Response& res) {
    res.status = 405;
    res.set_header("Allow", "POST");
  };
  http.Get("/mcp", reject_method);
  http.Delete("/mcp", reject_method);

  u16 bound_port = 0;
  for (u16 i = 0; i < PORT_ATTEMPTS; i++) {
    const auto candidate = static_cast<u16>(port + i);
    if (http.bind_to_port("127.0.0.1", candidate)) {
      bound_port = candidate;
      break;
    }
  }

  if (bound_port == 0) {
    return std::unexpected(
      fmt::format("could not bind 127.0.0.1 on ports {} to {}", port, static_cast<u16>(port + PORT_ATTEMPTS - 1))
    );
  }

  impl->thread = std::thread([raw = impl.get()] { raw->server.listen_after_bind(); });
  impl->server.wait_until_ready();

  self.impl = std::move(impl);
  return bound_port;
}

auto McpHttp::stop(this McpHttp& self) -> void {
  ZoneScoped;

  if (!self.impl) {
    return;
  }

  self.impl->server.stop();
  if (self.impl->thread.joinable()) {
    self.impl->thread.join();
  }

  self.impl.reset();
}
} // namespace ox
