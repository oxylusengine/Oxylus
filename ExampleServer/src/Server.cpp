#include "Server.hpp"

#include "Core/App.hpp"
#include "Networking/NetworkManager.hpp"

struct Player {
  u64 net_id = 0;
};

// We are doing a peak gamedev moment here
static auto player_material = ox::UUID::from_string("f884aa3f-5e57-4e84-bd10-b3b2f7dff29a").value();

GameServer::GameServer(ENetHost* local_host_, ox::Scene* scene_) : ox::NetServer(local_host_), scene(scene_) {
  this->register_proc("player_input", [this](ox::NetClientID client_id, std::span<ox::RPCParameter> params) {
    auto* client = remote_clients.slot(client_id);
    auto vel_x = params[0].as_f32().value_or(0.0f);
    auto vel_y = params[1].as_f32().value_or(0.0f);
    this->client_velocities[client->net_id] = glm::vec3(vel_x, vel_y, 0.0f);
  });
}

auto GameServer::on_client_connect(ox::NetClientID client_id) -> void {
  client_snapshots.emplace(client_id, ox::SceneSnapshotBuilder{});

  auto* client = remote_clients.slot(client_id);
  auto& world = scene->world;
  world.entity()
    .set<ox::TransformComponent>({.scale = {0.1f, 0.1f, 0.1f}}) //
    .set<ox::SpriteComponent>({.material = player_material})
    .set<Player>({.net_id = client->net_id});

  auto tick_state = ox::SceneState{};
  ox::SceneSnapshotBuilder::take_snapshot(scene->world, tick_state);
  if (auto delta_state_packet = ox::NetPacket::scene_snapshot(tick_state, 0)) {
    client->send_unreliable(*delta_state_packet);
  }
}

auto GameServer::on_client_disconnect(ox::NetClientID client_id) -> void {
  auto* client = remote_clients.slot(client_id);
  client_velocities.erase(client->net_id);
  client_snapshots.erase(client_id);
}

auto GameServer::on_client_ack(ox::NetClientID client_id, ox::NetClientAckPacket& packet) -> void {
  auto client_snapshot_it = client_snapshots.find(client_id);
  if (client_snapshot_it == client_snapshots.end()) {
    // TODO, BUG: This is very illegal, should probably report this?
    return;
  }

  auto& [_, state] = *client_snapshot_it;
  state.ack(packet.acked);
}

auto Server::init(this Server& self) -> std::expected<void, std::string> {
  self.scene = std::make_unique<ox::Scene>("MainScene");
  auto& world = self.scene->world;
  world.component<Player>("Player")
    .member<u64>("net_id") //
    .add<ox::Networked>();

  world.system<ox::TransformComponent, const Player>("player input")
    .each([&](flecs::iter& it, usize i, ox::TransformComponent& t, const Player& p) {
      auto velocities_it = self.server->client_velocities.find(p.net_id);
      if (velocities_it == self.server->client_velocities.end()) {
        return;
      }

      auto& velocity = velocities_it->second;
      t.position += velocity * it.delta_time();
    });

  self.scene->runtime_start();

  auto& net = ox::App::mod<ox::NetworkManager>();
  self.server = net.create_server<GameServer>(3131, 31, self.scene.get());
  self.server->set_tick_rate(20.0f);

  return {};
}

auto Server::deinit(this Server& self) -> std::expected<void, std::string> {
  auto& net = ox::App::mod<ox::NetworkManager>();
  net.destroy_server(self.server);

  self.scene->runtime_stop();

  return {};
}

auto Server::update(this Server& self, const ox::Timestep& timestep) -> void {
  self.scene->runtime_update(timestep);

  if (self.server->tick(timestep)) {
    auto tick_state = ox::SceneState{};
    ox::SceneSnapshotBuilder::take_snapshot(self.scene->world, tick_state);
    for (auto& [client_id, client_snapshot] : self.server->client_snapshots) {
      auto* client = self.server->remote_clients.slot(client_id);

      client_snapshot.set_current(tick_state);
      auto delta_state = client_snapshot.delta();
      if (auto delta_state_packet = ox::NetPacket::scene_snapshot(delta_state, client_snapshot.current_sequence)) {
        client->send_unreliable(*delta_state_packet);
      }

      client_snapshot.advance();
    }
  }
}
