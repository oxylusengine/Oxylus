#include "Game.hpp"

#include <Asset/AssetManager.hpp>
#include <Core/App.hpp>
#include <Core/Input.hpp>
#include <Core/Project.hpp>
#include <UI/ImGuiRenderer.hpp>
#include <UI/SceneHierarchyViewer.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <vuk/vsl/Core.hpp>

#include "Networking/NetworkManager.hpp"

struct Player {
  u64 net_id = 0;
};

struct PrevState {};
struct TargetState {};

struct InterpComponent {
  f32 time = 0.0f;
};

GameClient::GameClient(ENetHost* local_host_, ox::Scene* scene_) : ox::NetClient(local_host_), scene(scene_) {}

auto GameClient::on_scene_snapshot(u8 sequence, ox::SceneState&& state) -> void {
  auto& world = scene->world;
  world.enable_range_check(false);
  for (const auto& removed_entity : state.removed_entities) {
    auto e = world.get_alive(removed_entity);
    e.destruct();
  }

  for (const auto& [_, entity_state] : state.entities) {
    auto entity = flecs::entity(world, entity_state.entity_id);
    if (!entity.is_alive()) {
      entity = world.make_alive(entity_state.entity_id);
    }

    for (const auto& removed_component_id : entity_state.removed_components) {
      entity.remove(removed_component_id);
    }

    for (const auto& [_, component_state] : entity_state.components) {
      if (!entity.has(component_state.id)) {
        entity.add(component_state.id);
      }

      if (!component_state.buffer.empty()) {
        if (component_state.id == world.id<ox::TransformComponent>()) {
          auto* incoming_transform = reinterpret_cast<const ox::TransformComponent*>(component_state.buffer.data());
          if (entity.has<ox::TransformComponent>()) {
            // assign current to previous
            auto& current_transform = entity.get<ox::TransformComponent>();
            entity.set<ox::TransformComponent, PrevState>(current_transform);
          } else {
            // first time, set incoming directly
            entity.set<ox::TransformComponent, PrevState>(*incoming_transform);
          }

          entity.set<ox::TransformComponent, TargetState>(*incoming_transform);

          auto now = std::chrono::steady_clock::now();
          auto time_ms = std::chrono::duration<f32, std::milli>(now.time_since_epoch()).count();
          entity.set<InterpComponent>({.time = time_ms});
        }

        entity.set_ptr(component_state.id, component_state.buffer.size(), component_state.buffer.data());
      }

      entity.modified(component_state.id);
    }
  }

  if (auto p = ox::NetPacket::client_ack({.acked = sequence})) {
    send_unreliable(p.value());
  }
  world.enable_range_check(true);
}

auto Game::init(this Game& self) -> std::expected<void, std::string> {
  auto& net = ox::App::mod<ox::NetworkManager>();
  auto& vfs = ox::App::get_vfs();
  auto& asset_man = ox::App::mod<ox::AssetManager>();
  auto sprites_dir = vfs.resolve_physical_dir(vfs.APP_DIR, "Sprites");
  asset_man.import_asset(sprites_dir / "player.png");
  auto player_material = asset_man.import_asset(sprites_dir / "player_material.oxasset");
  asset_man.load_asset(player_material);

  self.main_scene = std::make_unique<ox::Scene>("MainScene");
  auto& world = self.main_scene->world;
  world.set_entity_range(1'000'000, 0);

  world.component<Player>("Player")
    .member<u64>("net_id") //
    .add<ox::Networked>();

  world.entity("camera")
    .set<ox::TransformComponent>(
      {.position = glm::vec3(0.0f, 0.0f, 5.0f),
       .rotation = glm::quat(glm::vec3(0.0f, glm::radians(-90.0f), 0.0f))}
    )
    .set<ox::CameraComponent>({.projection = ox::CameraComponent::Orthographic});

  // clang-format off
  world.system<ox::TransformComponent, const ox::TransformComponent, const ox::TransformComponent, const InterpComponent>("interp")
    .term_at(1).second<PrevState>()
    .term_at(2).second<TargetState>()
    .each([&](
      flecs::iter& it, usize i,
      ox::TransformComponent& t,
      const ox::TransformComponent& prev,
      const ox::TransformComponent& target,
      const InterpComponent& interp
    ) {
      // clang-format on
      auto e = it.entity(i);
      if (e.has<Player>()) {
        const auto& player = e.get<Player>();
        if (player.net_id == self.client->net_id) {
          e.remove<InterpComponent>();
          e.remove<ox::TransformComponent, PrevState>();
          e.remove<ox::TransformComponent, TargetState>();
          return;
        }
      }

      auto now = std::chrono::steady_clock::now();
      auto current_time = std::chrono::duration<f32, std::milli>(now.time_since_epoch()).count();
      auto elapsed = current_time - interp.time;
      auto tick_factor = glm::clamp(elapsed / static_cast<f32>(self.client->tick_interval), 0.0f, 1.0f);
      t.position = glm::mix(prev.position, target.position, tick_factor);

      e.modified<ox::TransformComponent>();

      if (tick_factor >= 1.0f) {
        e.remove<InterpComponent>();
      }
    });

  world.system<ox::TransformComponent, const Player>("player input")
    .each([&](flecs::iter& it, usize i, ox::TransformComponent& t, const Player& p) {
      auto e = it.entity(i);
      if (p.net_id != self.client->net_id) {
        return;
      }

      auto& input = ox::App::mod<ox::Input>();

      self.velocity = {};
      if (input.get_key_held(ox::KeyCode::W)) {
        self.velocity.y += 1.0f;
      }
      if (input.get_key_held(ox::KeyCode::S)) {
        self.velocity.y += -1.0f;
      }
      if (input.get_key_held(ox::KeyCode::D)) {
        self.velocity.x += 1.0f;
      }
      if (input.get_key_held(ox::KeyCode::A)) {
        self.velocity.x += -1.0f;
      }

      if (self.velocity != glm::vec3({})) {
        t.position += self.velocity * it.delta_time();
        e.modified<ox::TransformComponent>();
      }
    });

  self.main_scene->runtime_start();

  self.client = net.create_client<GameClient>(self.main_scene.get());
  self.client->set_tick_rate(20.0f);

  return {};
}

auto Game::deinit(this Game& self) -> std::expected<void, std::string> {
  ZoneScoped;

  auto& net = ox::App::mod<ox::NetworkManager>();
  net.destroy_client(self.client);

  self.main_scene->runtime_stop();

  return {};
}

auto Game::update(this Game& self, const ox::Timestep& timestep) -> void {
  ZoneScoped;

  auto& vk_context = ox::App::get_vkcontext();
  auto& imgui_renderer = ox::App::mod<ox::ImGuiRenderer>();
  auto& window = ox::App::get_window();

  if (self.client->tick(timestep) && self.client->status == ox::NetClientStatus::Connected) {
    ox::RPCParameter rpc_params[] = {
      {.type = ox::RPCParameter::Type::Float, .data_size = 4, .data = &self.velocity.x},
      {.type = ox::RPCParameter::Type::Float, .data_size = 4, .data = &self.velocity.y},
    };
    if (auto p = ox::NetPacket::rpc("player_input", rpc_params)) {
      self.client->send_unreliable(p.value());
    }
  }

  self.main_scene->runtime_update(timestep);

  imgui_renderer.begin_frame(timestep.get_seconds(), {window.get_logical_width(), window.get_logical_height()});
  ImGui::Begin("Network");
  switch (self.client->status) {
    case ox::NetClientStatus::None: {
      ImGui::TextUnformatted("Client state: idle");
    } break;
    case ox::NetClientStatus::Connecting: {
      ImGui::TextUnformatted("Client state: connecting...");
    } break;
    case ox::NetClientStatus::Connected: {
      ImGui::TextUnformatted("Client state: connected");
      ImGui::Text("NetID: %llu", self.client->net_id);
      auto& stats = self.client->stats;
      ImGui::Text(
        "Ping: %u\nRX: %u B/s\nTX: %u B/s\nLost packets: %u\nRTT: %u",
        stats.ping,
        stats.received_bytes,
        stats.sent_bytes,
        stats.packets_lost,
        stats.rtt
      );
    } break;
    case ox::NetClientStatus::Disconnected: {
      ImGui::TextUnformatted("Client state: disconnected");
    } break;
    case ox::NetClientStatus::TimedOut: {
      ImGui::TextUnformatted("Client state: timed out");
    } break;
  }

  static std::string ip = "127.0.0.1";
  static i32 port = 3131;
  ImGui::InputText("##ip", &ip);
  ImGui::SameLine();
  ImGui::InputInt("##port", &port);
  if (ImGui::Button("Connect")) {
    self.client->connect(ip, port, 5000.0f);
  }
  ImGui::End();

  auto swapchain_attachment = vk_context.new_frame();
  swapchain_attachment = vuk::clear_image(std::move(swapchain_attachment), vuk::Black<f32>);
  self.main_scene->on_render(swapchain_attachment->extent, swapchain_attachment->format);
  auto renderer_instance = self.main_scene->get_renderer_instance();
  const ox::Renderer::RenderInfo render_info = {};
  auto scene_view_image = renderer_instance->render(std::move(swapchain_attachment), render_info);

  scene_view_image = imgui_renderer.end_frame(vk_context, std::move(scene_view_image));

  vk_context.end_frame(scene_view_image);
}
