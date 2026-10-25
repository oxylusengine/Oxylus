#include <algorithm>
#include <ankerl/svector.h>
#include <array>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "Core/App.hpp"
#include "DotNetHosts.hpp"
#include "Memory/Stack.hpp"
#include "Networking/NetworkManager.hpp"
#include "Utils/Log.hpp"

namespace ox {
static auto servers = ankerl::svector<NetServer*, 2>{};
static auto clients = ankerl::svector<NetClient*, 2>{};
// a script may close an endpoint from a callback its own tick raised, which waits for the tick to end
static auto closing = ankerl::svector<u64, 2>{};
static auto is_ticking = false;
static auto handlers = std::array<HandlerId, 6>{};
static auto is_subscribed = false;

static auto to_view(OxStringView view) -> std::string_view { return std::string_view(view.data, view.length); }

static auto network() -> NetworkManager* {
  return App::get() && App::has_mod<NetworkManager>() ? &App::mod<NetworkManager>() : nullptr;
}

static auto server_of(u64 endpoint) -> NetServer* {
  const auto it = std::ranges::find(servers, reinterpret_cast<NetServer*>(endpoint));
  return it == servers.end() ? nullptr : *it;
}

static auto client_of(u64 endpoint) -> NetClient* {
  const auto it = std::ranges::find(clients, reinterpret_cast<NetClient*>(endpoint));
  return it == clients.end() ? nullptr : *it;
}

static auto handle_of(const void* endpoint) -> u64 { return reinterpret_cast<u64>(endpoint); }

static auto forward_snapshot(const ClientSceneSnapshotEvent& event) -> void {
  const auto* managed = managed_api();
  if (!managed || !client_of(handle_of(event.client))) {
    return;
  }

  memory::ScopedStack stack;
  auto changed = stack.alloc<u64>(event.scene_state.entities.size());
  auto i = 0_sz;
  for (const auto& entity : event.scene_state.entities) {
    changed[i++] = entity.first;
  }

  auto removed = stack.alloc<u64>(event.scene_state.removed_entities.size());
  i = 0;
  for (const auto entity : event.scene_state.removed_entities) {
    removed[i++] = entity;
  }

  managed->net_snapshot(
    handle_of(event.client),
    event.sequence,
    changed.data(),
    static_cast<i32>(changed.size()),
    removed.data(),
    static_cast<i32>(removed.size())
  );
}

// the bus is global, only the endpoints the scripts opened are theirs to hear about
static auto subscribe() -> void {
  if (is_subscribed) {
    return;
  }

  auto& events = App::get_event_system();
  const auto notify = [](const void* endpoint, OxNetEvent event, u64 peer, u32 reason) {
    if (const auto* managed = managed_api()) {
      managed->net_event(handle_of(endpoint), event, peer, reason);
    }
  };

  handlers[0] = events
                  .subscribe<ClientConnectEvent>([notify](const ClientConnectEvent& event) {
                    if (server_of(handle_of(event.server))) {
                      notify(event.server, OxNetEvent::ClientConnected, static_cast<u64>(event.client_id), 0);
                    }
                  })
                  .value_or(HandlerId{});
  handlers[1] = events
                  .subscribe<ClientDisconnectEvent>([notify](const ClientDisconnectEvent& event) {
                    if (server_of(handle_of(event.server))) {
                      notify(event.server, OxNetEvent::ClientDisconnected, static_cast<u64>(event.client_id), 0);
                    }
                  })
                  .value_or(HandlerId{});
  handlers[2] = events
                  .subscribe<ServerConnectEvent>([notify](const ServerConnectEvent& event) {
                    if (client_of(handle_of(event.client))) {
                      notify(event.client, OxNetEvent::Connected, event.net_id, 0);
                    }
                  })
                  .value_or(HandlerId{});
  handlers[3] = events
                  .subscribe<ServerDisconnectEvent>([notify](const ServerDisconnectEvent& event) {
                    if (client_of(handle_of(event.client))) {
                      notify(event.client, OxNetEvent::Disconnected, 0, static_cast<u32>(event.reason));
                    }
                  })
                  .value_or(HandlerId{});
  handlers[4] = events
                  .subscribe<ClientAckEvent>([notify](const ClientAckEvent& event) {
                    if (server_of(handle_of(event.server))) {
                      notify(
                        event.server,
                        OxNetEvent::ClientAcked,
                        static_cast<u64>(event.client_id),
                        event.packet.acked
                      );
                    }
                  })
                  .value_or(HandlerId{});
  handlers[5] = events.subscribe<ClientSceneSnapshotEvent>(forward_snapshot).value_or(HandlerId{});
  is_subscribed = true;
}

static auto unsubscribe() -> void {
  if (!is_subscribed || !App::get()) {
    return;
  }

  auto& events = App::get_event_system();
  std::ignore = events.unsubscribe<ClientConnectEvent>(handlers[0]);
  std::ignore = events.unsubscribe<ClientDisconnectEvent>(handlers[1]);
  std::ignore = events.unsubscribe<ServerConnectEvent>(handlers[2]);
  std::ignore = events.unsubscribe<ServerDisconnectEvent>(handlers[3]);
  std::ignore = events.unsubscribe<ClientAckEvent>(handlers[4]);
  std::ignore = events.unsubscribe<ClientSceneSnapshotEvent>(handlers[5]);
  is_subscribed = false;
}

static auto to_parameter(const OxValue& value) -> option<RPCParameter> {
  const auto bytes = std::span(reinterpret_cast<const u8*>(value.text.data), value.text.length);
  switch (value.type) {
    case OxValueType::Bool  :
    case OxValueType::U8    : return RPCParameter{.value = static_cast<u8>(value.integer)};
    case OxValueType::U16   : return RPCParameter{.value = static_cast<u16>(value.integer)};
    case OxValueType::I32   : return RPCParameter{.value = static_cast<i32>(value.integer)};
    case OxValueType::I64   : return RPCParameter{.value = value.integer};
    case OxValueType::F32   : return RPCParameter{.value = static_cast<f32>(value.number)};
    case OxValueType::F64   : return RPCParameter{.value = value.number};
    case OxValueType::String: return RPCParameter{.value = std::string(to_view(value.text))};
    case OxValueType::Bytes : return RPCParameter{.value = std::vector<u8>(bytes.begin(), bytes.end())};
    case OxValueType::Uuid:
      if (bytes.size() != 16) {
        return nullopt;
      }

      {
        auto uuid = std::array<u8, 16>{};
        std::memcpy(uuid.data(), bytes.data(), 16);
        return RPCParameter{.value = uuid};
      }
    case OxValueType::None: return RPCParameter{};
  }

  return nullopt;
}

static auto to_value(const RPCParameter& parameter) -> OxValue {
  return std::visit(
    [](const auto& value) -> OxValue {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, u8>) {
        return OxValue{.type = OxValueType::U8, .integer = value};
      } else if constexpr (std::is_same_v<T, u16>) {
        return OxValue{.type = OxValueType::U16, .integer = value};
      } else if constexpr (std::is_same_v<T, i32>) {
        return OxValue{.type = OxValueType::I32, .integer = value};
      } else if constexpr (std::is_same_v<T, i64>) {
        return OxValue{.type = OxValueType::I64, .integer = value};
      } else if constexpr (std::is_same_v<T, f32>) {
        return OxValue{.type = OxValueType::F32, .number = value};
      } else if constexpr (std::is_same_v<T, f64>) {
        return OxValue{.type = OxValueType::F64, .number = value};
      } else if constexpr (std::is_same_v<T, std::string>) {
        return OxValue{.type = OxValueType::String, .text = {.data = value.data(), .length = value.size()}};
      } else if constexpr (std::is_same_v<T, std::array<u8, 16>>) {
        return OxValue{
          .type = OxValueType::Uuid,
          .text = {.data = reinterpret_cast<const c8*>(value.data()), .length = value.size()}
        };
      } else if constexpr (std::is_same_v<T, std::vector<u8>>) {
        return OxValue{
          .type = OxValueType::Bytes,
          .text = {.data = reinterpret_cast<const c8*>(value.data()), .length = value.size()}
        };
      } else {
        return OxValue{};
      }
    },
    parameter.value
  );
}

static auto forward_rpc(u64 endpoint, i32 binding, u64 sender, std::span<RPCParameter> parameters) -> void {
  const auto* managed = managed_api();
  if (!managed) {
    return;
  }

  memory::ScopedStack stack;
  auto values = stack.alloc<OxValue>(parameters.size());
  for (usize i = 0; i < parameters.size(); i++) {
    values[i] = to_value(parameters[i]);
  }

  managed->net_rpc(endpoint, binding, sender, values.data(), static_cast<i32>(values.size()));
}

template <typename T>
static auto erase_one(ankerl::svector<T*, 2>& list, T* item) -> void {
  if (const auto it = std::ranges::find(list, item); it != list.end()) {
    list.erase(it);
  }
}

static auto close_endpoint(u64 endpoint) -> void {
  auto* net = network();
  if (auto* server = server_of(endpoint)) {
    erase_one(servers, server);
    if (net) {
      net->destroy_server(server);
    }
  } else if (auto* client = client_of(endpoint)) {
    erase_one(clients, client);
    if (net) {
      net->destroy_client(client);
    }
  }
}

static auto net_server_start(u16 port, u32 max_clients, u64* server) -> OxStatus {
  ZoneScoped;

  *server = 0;
  auto* net = network();
  if (!net) {
    return OxStatus::NotFound;
  }

  auto* created = net->create_server(port, max_clients);
  if (!created) {
    return OxStatus::InvalidArgument;
  }

  subscribe();
  servers.push_back(created);
  *server = handle_of(created);
  return OxStatus::Ok;
}

static auto net_client_connect(OxStringView host, u16 port, u32 timeout_ms, u64* client) -> OxStatus {
  ZoneScoped;

  *client = 0;
  auto* net = network();
  if (!net) {
    return OxStatus::NotFound;
  }

  auto* created = net->create_client();
  if (!created) {
    return OxStatus::InvalidArgument;
  }

  if (!created->connect(to_view(host), port, static_cast<f64>(timeout_ms))) {
    net->destroy_client(created);
    return OxStatus::InvalidArgument;
  }

  subscribe();
  clients.push_back(created);
  *client = handle_of(created);
  return OxStatus::Ok;
}

static auto net_close(u64 endpoint) -> void {
  if (is_ticking) {
    closing.push_back(endpoint);
    return;
  }

  close_endpoint(endpoint);
}

static auto net_client_state(u64 client, OxNetClientState* state) -> OxStatus {
  *state = {};
  const auto* found = client_of(client);
  if (!found) {
    return OxStatus::NotFound;
  }

  *state = OxNetClientState{
    .status = static_cast<u32>(found->status),
    .ping = found->stats.ping,
    .net_id = found->net_id,
    .rtt = found->stats.rtt,
    .packets_lost = found->stats.packets_lost,
    .sent_bytes = found->stats.sent_bytes,
    .received_bytes = found->stats.received_bytes,
  };
  return OxStatus::Ok;
}

static auto net_server_clients(u64 server, u64* ids, i32 capacity) -> i32 {
  auto* found = server_of(server);
  if (!found) {
    return 0;
  }

  const auto client_ids = found->client_ids();
  for (i32 i = 0; i < std::min(static_cast<i32>(client_ids.size()), capacity); i++) {
    ids[i] = static_cast<u64>(client_ids[static_cast<usize>(i)]);
  }

  return static_cast<i32>(client_ids.size());
}

static auto net_register_rpc(u64 endpoint, OxStringView name, i32 binding) -> OxStatus {
  ZoneScoped;

  if (auto* server = server_of(endpoint)) {
    server->register_proc(to_view(name), [endpoint, binding](NetClientID sender, std::span<RPCParameter> parameters) {
      forward_rpc(endpoint, binding, static_cast<u64>(sender), parameters);
    });
    return OxStatus::Ok;
  }

  if (auto* client = client_of(endpoint)) {
    client->register_proc(to_view(name), [endpoint, binding](NetClientID, std::span<RPCParameter> parameters) {
      forward_rpc(endpoint, binding, static_cast<u64>(NetClientID::Invalid), parameters);
    });
    return OxStatus::Ok;
  }

  return OxStatus::NotFound;
}

static auto net_call(u64 endpoint, u64 client, OxStringView name, const OxValue* args, i32 count, bool is_reliable)
  -> OxStatus {
  ZoneScoped;

  auto parameters = ankerl::svector<RPCParameter, 8>();
  for (const auto& arg : std::span(args, static_cast<usize>(std::max(count, 0)))) {
    auto parameter = to_parameter(arg);
    if (!parameter) {
      return OxStatus::InvalidArgument;
    }

    parameters.push_back(std::move(*parameter));
  }

  auto is_sent = false;
  if (auto* server = server_of(endpoint)) {
    is_sent = client == static_cast<u64>(NetClientID::Invalid)
                ? server->broadcast_call(to_view(name), parameters, is_reliable)
                : server->call_client(static_cast<NetClientID>(client), to_view(name), parameters, is_reliable);
  } else if (auto* found = client_of(endpoint)) {
    is_sent = found->call_server(to_view(name), parameters, is_reliable);
  } else {
    return OxStatus::NotFound;
  }

  return is_sent ? OxStatus::Ok : OxStatus::InvalidArgument;
}

auto tick_network(const Timestep& timestep) -> void {
  ZoneScoped;

  if (servers.empty() && clients.empty()) {
    return;
  }

  // copied, a callback can open another endpoint and grow the lists under the loop
  is_ticking = true;
  const auto ticked_servers = servers;
  const auto ticked_clients = clients;
  for (auto* server : ticked_servers) {
    server->tick(timestep);
  }

  for (auto* client : ticked_clients) {
    client->tick(timestep);
  }

  is_ticking = false;
  for (const auto endpoint : closing) {
    close_endpoint(endpoint);
  }

  closing.clear();
}

auto release_network() -> void {
  ZoneScoped;

  closing.clear();
  while (!servers.empty()) {
    close_endpoint(handle_of(servers.back()));
  }

  while (!clients.empty()) {
    close_endpoint(handle_of(clients.back()));
  }

  unsubscribe();
}

auto fill_net_api(OxNativeApi& api) -> void {
  api.net_server_start = net_server_start;
  api.net_client_connect = net_client_connect;
  api.net_close = net_close;
  api.net_client_state = net_client_state;
  api.net_server_clients = net_server_clients;
  api.net_register_rpc = net_register_rpc;
  api.net_call = net_call;
}
} // namespace ox
