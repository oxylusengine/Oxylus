#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "Core/Types.hpp"

namespace ox {
class McpServer;

class McpHttp {
public:
  McpHttp();
  ~McpHttp();

  auto start(this McpHttp& self, McpServer& server, u16 port, std::string token, std::string_view version)
    -> std::expected<u16, std::string>;
  auto stop(this McpHttp& self) -> void;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
} // namespace ox
