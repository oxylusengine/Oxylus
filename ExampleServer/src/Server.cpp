#include "Server.hpp"

#include "Core/App.hpp"
#include "Networking/NetworkManager.hpp"

auto Server::init(this Server& self) -> std::expected<void, std::string> {
  auto& net = ox::App::mod<ox::NetworkManager>();
  self.server = net.create_server(3131, 31);

  return {};
}

auto Server::deinit(this Server& self) -> std::expected<void, std::string> {
  auto& net = ox::App::mod<ox::NetworkManager>();
  net.destroy_server(self.server);

  return {};
}

auto Server::update(this Server& self, const ox::Timestep& timestep) -> void {}
