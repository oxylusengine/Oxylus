#include "Scripting/LuaNetworkBindings.hpp"

#include <sol/state.hpp>

#include "Networking/NetworkManager.hpp"
#include "Utils/Log.hpp"

namespace ox {
// RPC parameters cross into Lua as plain values instead of a usertype, so scripts never hold an
// ENetPacket and packet lifetime stays in C++.
auto rpc_params_from_lua(const sol::optional<sol::table>& params) -> ankerl::svector<RPCParameter, 8> {
  ZoneScoped;

  auto values = ankerl::svector<RPCParameter, 8>{};
  if (!params.has_value()) {
    return values;
  }

  const auto& table = params.value();
  const auto count = table.size();
  values.reserve(count);

  for (usize i = 1; i <= count; i++) {
    auto value = table.get<sol::object>(i);
    switch (value.get_type()) {
      case sol::type::number: {
        // Lua 5.4 tracks the integer subtype, keep it so ints don't arrive as floats.
        auto* L = value.lua_state();
        value.push(L);
        const auto is_integer = lua_isinteger(L, -1) != 0;
        lua_pop(L, 1);

        if (is_integer) {
          values.emplace_back(RPCParameter{.value = value.as<i64>()});
        } else {
          values.emplace_back(RPCParameter{.value = value.as<f64>()});
        }
      } break;
      case sol::type::string : values.emplace_back(RPCParameter{.value = value.as<std::string>()}); break;
      case sol::type::boolean: values.emplace_back(RPCParameter{.value = static_cast<u8>(value.as<bool>())}); break;
      default                : {
        OX_LOG_ERROR("RPC parameter {} has an unsupported type, sending it as none.", i);
        values.emplace_back(RPCParameter{});
      } break;
    }
  }

  return values;
}

auto rpc_params_to_lua(sol::state_view lua, std::span<RPCParameter> params) -> sol::table {
  ZoneScoped;

  auto table = lua.create_table(static_cast<int>(params.size()), 0);
  for (usize i = 0; i < params.size(); i++) {
    std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          table[i + 1] = sol::lua_nil;
        } else if constexpr (std::is_same_v<T, std::array<u8, 16>>) {
          auto bytes = value;
          auto uuid = UUID::from_bytes(bytes);
          table[i + 1] = uuid.has_value() ? uuid->str() : std::string{};
        } else if constexpr (std::is_same_v<T, std::vector<u8>>) {
          table[i + 1] = std::string(reinterpret_cast<const c8*>(value.data()), value.size());
        } else {
          table[i + 1] = value;
        }
      },
      params[i].value
    );
  }

  return table;
}

// `client_id` is nil when the call came from the server, since a server has no client id.
auto make_rpc_callback(sol::protected_function fn) -> NetRPCPacket::Callback {
  return [callback = std::move(fn)](NetClientID client_id, std::span<RPCParameter> params) {
    ZoneScopedN("Lua RPC callback");

    auto lua = sol::state_view(callback.lua_state());
    auto id = client_id == NetClientID::Invalid ? sol::object(sol::lua_nil)
                                                : sol::make_object(lua, static_cast<u64>(client_id));

    auto result = callback(id, rpc_params_to_lua(lua, params));
    if (!result.valid()) {
      const sol::error err = result;
      OX_LOG_ERROR("An RPC callback failed: {}", err.what());
    }
  };
}

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

  state->new_enum<NetClientStatus>(
    "NetClientStatus",
    {
      {"None", NetClientStatus::None},
      {"Connecting", NetClientStatus::Connecting},
      {"Connected", NetClientStatus::Connected},
      {"Disconnected", NetClientStatus::Disconnected},
      {"TimedOut", NetClientStatus::TimedOut},
    }
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
    [](NetServer* self, std::string_view proc, sol::protected_function fn) -> void {
      self->register_proc(proc, make_rpc_callback(std::move(fn)));
    },

    "get_client",
    [](NetServer* self, u64 client_id) -> NetClient* { return self->client(static_cast<NetClientID>(client_id)); },

    "get_client_ids",
    [](NetServer* self, sol::this_state lua_state) -> sol::table {
      const auto ids = self->client_ids();
      auto lua = sol::state_view(lua_state);
      auto table = lua.create_table(static_cast<int>(ids.size()), 0);
      for (usize i = 0; i < ids.size(); i++) {
        table[i + 1] = static_cast<u64>(ids[i]);
      }

      return table;
    },

    "call_client",
    [](
      NetServer* self,
      u64 client_id,
      std::string_view proc,
      sol::optional<sol::table> params,
      sol::optional<bool> reliable
    ) -> bool {
      const auto values = rpc_params_from_lua(params);
      return self->call_client(static_cast<NetClientID>(client_id), proc, values, reliable.value_or(true));
    },

    "broadcast",
    [](NetServer* self, std::string_view proc, sol::optional<sol::table> params, sol::optional<bool> reliable) -> bool {
      const auto values = rpc_params_from_lua(params);
      return self->broadcast_call(proc, values, reliable.value_or(true));
    }
  );

  state->new_usertype<NetClient>(
    "NetClient",

    "stats",
    &NetClient::stats,

    "status",
    sol::readonly(&NetClient::status),

    "net_id",
    sol::readonly(&NetClient::net_id),

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
    [](NetClient* self, std::string_view proc, sol::protected_function fn) -> void {
      self->register_proc(proc, make_rpc_callback(std::move(fn)));
    },

    "call_server",
    [](NetClient* self, std::string_view proc, sol::optional<sol::table> params, sol::optional<bool> reliable) -> bool {
      const auto values = rpc_params_from_lua(params);
      return self->call_server(proc, values, reliable.value_or(true));
    },

    // NetPacket is deliberately not bound; these two stay unreachable from Lua, use call_server instead.
    "send_reliable",
    &NetClient::send_reliable,

    "send_unreliable",
    &NetClient::send_unreliable
  );
}
} // namespace ox
