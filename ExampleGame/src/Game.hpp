#pragma once

#include "Networking/NetClient.hpp"
#include "Scene/Scene.hpp"

struct GameClient : ox::NetClient {
  ox::Scene* scene = nullptr;

  GameClient(ENetHost* local_host_, ox::Scene* scene_);

  auto on_scene_snapshot(u8 sequence, ox::SceneState&& state) -> void override;
};

class Game {
public:
  constexpr static auto MODULE_NAME = "Game";

  auto init(this Game&) -> std::expected<void, std::string>;
  auto deinit(this Game&) -> std::expected<void, std::string>;
  auto update(this Game&, const ox::Timestep& timestep) -> void;

  std::unique_ptr<ox::Scene> main_scene = nullptr;
  GameClient* client = nullptr;
  glm::vec3 velocity = {};
};
