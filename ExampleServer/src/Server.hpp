#pragma once

#include <expected>
#include <string>

#include "Networking/NetServer.hpp"

#include "Utils/Timestep.hpp"

struct Server {
  constexpr static auto MODULE_NAME = "Server";

  ox::NetServer* server = nullptr;

  auto init(this Server& self) -> std::expected<void, std::string>;
  auto deinit(this Server& self) -> std::expected<void, std::string>;

  auto update(this Server& self, const ox::Timestep& timestep) -> void;
};
