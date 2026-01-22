#pragma once

#include <expected>
#include <string>

#include "Networking/NetServer.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Timestep.hpp"

struct GameServer : ox::NetServer {
  ox::Scene* scene = nullptr;
  ankerl::unordered_dense::map<ox::NetClientID, ox::SceneSnapshotBuilder> client_snapshots = {};
  ankerl::unordered_dense::map<u64, glm::vec3> client_velocities = {};
  ankerl::unordered_dense::map<ox::NetClientID, flecs::entity> client_entities = {};

  GameServer(ENetHost* local_host_, ox::Scene* scene_);
  auto on_client_connect(ox::NetClientID client_id) -> void override;
  auto on_client_disconnect(ox::NetClientID client_id) -> void override;
  auto on_client_ack(ox::NetClientID client_id, ox::NetClientAckPacket& packet) -> void override;
};

struct Server {
  constexpr static auto MODULE_NAME = "Server";

  GameServer* server = nullptr;

  auto init(this Server& self) -> std::expected<void, std::string>;
  auto deinit(this Server& self) -> std::expected<void, std::string>;

  auto update(this Server& self, const ox::Timestep& timestep) -> void;

  std::unique_ptr<ox::Scene> scene = nullptr;
};
