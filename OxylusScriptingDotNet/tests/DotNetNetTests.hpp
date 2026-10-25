#pragma once

// a server and a client opened from C# in one process, over localhost, stepped the way a game steps them: the
// scripting module ticks both every frame. Each test binary defines smoke_game() first

#include <chrono>
#include <flecs.h>
#include <format>
#include <gtest/gtest.h>
#include <thread>

#include "Core/App.hpp"
#include "DotNetTestHelpers.hpp"
#include "Networking/NetPacket.hpp"
#include "Networking/NetworkManager.hpp"
#include "Scene/Components.hpp"
#include "Utils/Timestep.hpp"

// SmokeGame.NetProbe, as flecs lays out its members
struct NetProbe {
  i32 phase = 0;
  i32 port = 0;
};

class DotNetNetTest : public ::testing::Test {
protected:
  void SetUp() override {
    static char arg0[] = "TestDotNetNet";
    static char* argv[] = {arg0, nullptr};
    loguru::g_stderr_verbosity = loguru::Verbosity_WARNING;
    captured_logs.clear();
    app = std::make_unique<ox::App>(1, argv);
    app->with<ox::NetworkManager>();
    app->init();

    world.import<ox::CoreComponentsModule>();
    const auto loaded = scripting.load(smoke_game(), capturing_native_api());
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    ASSERT_TRUE(scripting.attach_world(world.c_ptr()).has_value());

    net_probe = ecs_lookup(world.c_ptr(), "SmokeGame.NetProbe");
    ASSERT_NE(net_probe, 0);
    probe = world.entity();
    // a port per host, xmake test may run both binaries at once
    const auto initial = NetProbe{.port = smoke_game().kind == ox::DotNetHostKind::HostFxr ? 47321 : 47322};
    ecs_set_id(world.c_ptr(), probe, net_probe, sizeof(NetProbe), &initial);
  }

  void TearDown() override {
    scripting.detach_world(world.c_ptr());
    // closes whatever the scripts still have open, NetworkManager wants no server or client left at its deinit
    scripting.unload();
    app->stop();
    app.reset();
  }

  auto frame() -> void {
    scripting.update(ox::Timestep(16.0));
    world.progress(1.0f / 60.0f);
  }

  // frames until `message` is logged, a localhost round trip is a few of them
  auto frames_until(std::string_view message) -> bool {
    for (auto i = 0; i < 300; i++) {
      if (count_logs(ox::OxLogLevel::Info, message) > 0) {
        return true;
      }

      frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return false;
  }

  auto set_phase(i32 phase) -> void {
    auto* seen = static_cast<NetProbe*>(ecs_get_mut_id(world.c_ptr(), probe, net_probe));
    seen->phase = phase;
  }

  std::unique_ptr<ox::App> app = nullptr;
  flecs::world world;
  ox::DotNetScripting scripting = {};
  flecs::entity_t net_probe = 0;
  flecs::entity probe = {};
};

TEST_F(DotNetNetTest, ConnectsAndCallsTypedRpcsBothWays) {
  frame();
  auto& network = app->mod<ox::NetworkManager>();
  EXPECT_EQ(network.servers.size(), 1);
  EXPECT_EQ(network.clients.size(), 1);

  ASSERT_TRUE(frames_until("client got 1.5 3 bytes 7")) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "server saw a client connect"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "client connected Connected"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "server got 42 hi"), 1);
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetNetTest, ReportsAcksAndSceneSnapshotsOverTheWire) {
  frame();
  ASSERT_TRUE(frames_until("client got 1.5 3 bytes 7")) << logs_text();
  auto& network = app->mod<ox::NetworkManager>();
  auto* server = network.servers.front().get();
  auto* client = network.clients.front().get();
  const auto client_id = server->client_ids().front();

  // the C# server hears the ack its client sends, carrying who sent it and what it acknowledged
  auto ack = ox::NetPacket::client_ack({.acked = 5});
  ASSERT_TRUE(ack.has_value());
  client->send_reliable(*ack);
  ASSERT_TRUE(frames_until(std::format("server got ack 5 from {}", static_cast<u64>(client_id)))) << logs_text();

  auto state = ox::SceneState{};
  state.entities[300] = ox::EntityState{.entity_id = 300};
  state.entities[100] = ox::EntityState{.entity_id = 100};
  state.removed_entities = {11, 7};
  auto snapshot = ox::NetPacket::scene_snapshot(state, 3);
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(server->send_to_client(client_id, *snapshot, true));
  ASSERT_TRUE(frames_until("client got snapshot 3 changed 100,300 removed 7,11")) << logs_text();

  // an empty one reaches it too
  auto empty = ox::NetPacket::scene_snapshot(ox::SceneState{}, 4);
  ASSERT_TRUE(empty.has_value());
  ASSERT_TRUE(server->send_to_client(client_id, *empty, true));
  ASSERT_TRUE(frames_until("client got snapshot 4 changed  removed ")) << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Error), 0) << logs_text();
}

TEST_F(DotNetNetTest, OnlyHearsAcksAndSnapshotsOfItsOwnEndpoints) {
  frame();
  ASSERT_TRUE(frames_until("client got 1.5 3 bytes 7")) << logs_text();
  auto& network = app->mod<ox::NetworkManager>();
  auto& events = ox::App::get_event_system();

  // endpoints the scripts never opened, raising the same events on the same bus
  auto foreign_server = ox::NetServer(nullptr);
  auto foreign_client = ox::NetClient(nullptr);
  const auto client_id = network.servers.front()->client_ids().front();

  auto state = ox::SceneState{};
  state.entities[42] = ox::EntityState{.entity_id = 42};

  std::ignore = events.emit(ox::ClientAckEvent{&foreign_server, client_id, {.acked = 9}});
  std::ignore = events.emit(ox::ClientSceneSnapshotEvent{&foreign_client, 8, state});
  EXPECT_EQ(count_logs_containing(ox::OxLogLevel::Info, "server got ack 9"), 0) << logs_text();
  EXPECT_EQ(count_logs_containing(ox::OxLogLevel::Info, "client got snapshot 8"), 0) << logs_text();

  // and the ones it did open are heard
  std::ignore = events.emit(ox::ClientAckEvent{network.servers.front().get(), client_id, {.acked = 4}});
  std::ignore = events.emit(ox::ClientSceneSnapshotEvent{network.clients.front().get(), 6, state});
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, std::format("server got ack 4 from {}", static_cast<u64>(client_id))), 1)
    << logs_text();
  EXPECT_EQ(count_logs(ox::OxLogLevel::Info, "client got snapshot 6 changed 42 removed "), 1) << logs_text();
}

TEST_F(DotNetNetTest, ClosesEndpointsFromCSharp) {
  frame();
  ASSERT_TRUE(frames_until("client got 1.5 3 bytes 7")) << logs_text();

  set_phase(2);
  ASSERT_TRUE(frames_until("server saw a client disconnect")) << logs_text();
  EXPECT_TRUE(app->mod<ox::NetworkManager>().clients.empty());

  set_phase(4);
  frame();
  EXPECT_TRUE(app->mod<ox::NetworkManager>().servers.empty());
}

TEST_F(DotNetNetTest, ClosesWhatTheScriptsLeftOpenWhenTheyUnload) {
  frame();
  ASSERT_TRUE(frames_until("client got 1.5 3 bytes 7")) << logs_text();

  scripting.unload();
  EXPECT_TRUE(app->mod<ox::NetworkManager>().servers.empty());
  EXPECT_TRUE(app->mod<ox::NetworkManager>().clients.empty());
}
