#include "Scripting/LuaNetworkBindings.hpp"

#include <sol/state.hpp>

#include "Networking/NetworkManager.hpp"

namespace ox {
auto NetworkBinding::bind(sol::state* state) -> void {
  ZoneScoped;

  state->new_usertype<NetStats>(
    "NetStats",

    "ping",
    &NetStats::ping,

    "sent_bytes",
    &NetStats::sent_bytes,

    "received_bytes",
    &NetStats::received_bytes,

    "sent_packets",
    &NetStats::sent_packets,

    "packets_lost",
    &NetStats::packets_lost,

    "rtt",
    &NetStats::rtt,

    "last_sent_bytes",
    &NetStats::last_sent_bytes,

    "last_received_bytes",
    &NetStats::last_received_bytes,

    "last_sent_packets",
    &NetStats::last_sent_packets
  );

  state->new_usertype<NetworkManager>(
    "NetworkManager",

    "create_server",
    [](NetworkManager* self, u16 port, u32 max_clients) -> NetServer* {
      return self->create_server(port, max_clients);
    },

    "create_client",
    [](NetworkManager* self) -> NetClient* { return self->create_client(); },

    "destroy_server",
    &NetworkManager::destroy_server,

    "destroy_client",
    &NetworkManager::destroy_client
  );

  state->new_usertype<NetServer>(
    "NetServer",

    "set_tick_rate",
    &NetServer::set_tick_rate,

    "tick",
    &NetServer::tick,

    "handle_packet",
    &NetServer::handle_packet,

    "register_proc",
    &NetServer::register_proc
  );

  state->new_usertype<NetClient>(
    "NetClient",

    "set_tick_rate",
    &NetClient::set_tick_rate,

    "connect",
    &NetClient::connect,

    "disconnect",
    &NetClient::disconnect,

    "tick",
    &NetClient::tick,

    "handle_packet",
    &NetClient::handle_packet,

    "add_builtin_procs",
    &NetClient::add_builtin_procs,

    "register_proc",
    &NetClient::register_proc,

    "send_reliable",
    &NetClient::send_reliable,

    "send_unreliable",
    &NetClient::send_unreliable
  );
}
} // namespace ox
