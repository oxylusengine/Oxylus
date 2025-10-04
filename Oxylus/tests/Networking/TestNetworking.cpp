#include <chrono>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../OxHelpers.hpp"
#include "Core/Types.hpp"
#include "Networking/Client.hpp"
#include "Networking/NetworkManager.hpp"
#include "Networking/Server.hpp"

class NetworkingTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_INFO;

    this->network_manager = std::make_unique<ox::NetworkManager>();
    auto network_manager_init_result = network_manager->init();
    if (!network_manager_init_result.has_value()) {
      OX_LOG_ERROR("{}", network_manager_init_result.error());
    }
    EXPECT_TRUE(network_manager_init_result.has_value());

    this->test_packet_client = std::make_unique<ox::Packet>(packet_id);
    this->test_packet_client->add_string(packet_string_value_client).add<ox::u32>(packet_u32_value);

    this->test_packet_server = std::make_unique<ox::Packet>(packet_id);
    this->test_packet_server->add_string(packet_string_value_server).add<ox::u32>(packet_u32_value);
  }

  void TearDown() override {
    auto _ = network_manager->deinit();
    log_test_end();
  }

  // Update both client and server for a duration/until condition is met
  template <typename Condition>
  auto update_until(
    ox::Client& client,
    ox::Server& server,
    Condition condition,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)
  ) -> bool {
    auto start = std::chrono::steady_clock::now();
    while (!condition()) {
      client.update();
      server.update();

      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed >= timeout) {
        return false;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  std::unique_ptr<ox::NetworkManager> network_manager = nullptr;
  std::unique_ptr<ox::Packet> test_packet_client = nullptr;
  std::unique_ptr<ox::Packet> test_packet_server = nullptr;
  static constexpr auto packet_string_value_client = "Hi from oxylus client!";
  static constexpr auto packet_string_value_server = "Hi from oxylus server!";
  static constexpr ox::u32 packet_u32_value = 31;
  static constexpr ox::u32 packet_id = 0;
};

TEST_F(NetworkingTest, TestServerClient) {
  const ox::u16 test_port = 7777;
  ox::Server server = {};
  auto server_start_result = server.set_port(test_port).set_max_clients(2).start();

  ASSERT_TRUE(server_start_result.has_value()) << server_start_result.error();
  EXPECT_TRUE(server.is_running());

  ox::Client client = {};
  auto connect_result = client.set_connect_timeout(1000).set_disconnect_timeout(1000).connect_async(
    "127.0.0.1",
    test_port
  );

  ASSERT_TRUE(connect_result.has_value()) << connect_result.error();

  // Update both until connected
  bool connected = update_until(client, server, [&]() { return client.is_connected() && server.get_peer_count() > 0; });

  EXPECT_TRUE(connected) << "Client failed to connect";
  EXPECT_TRUE(client.is_connected());
  EXPECT_EQ(server.get_peer_count(), 1);

  // Disconnect
  auto disconnect_result = client.disconnect();
  EXPECT_TRUE(disconnect_result.has_value());
  EXPECT_FALSE(client.is_connected());

  // Update both to process disconnection
  update_until(client, server, [&]() { return server.get_peer_count() == 0; }, std::chrono::milliseconds(500));

  EXPECT_EQ(server.get_peer_count(), 0);

  auto server_stop_result = server.stop();
  EXPECT_TRUE(server_stop_result.has_value());
}

TEST_F(NetworkingTest, TestServerReceivePacket) {
  struct SEH : public ox::ServerEventHandler {
    auto on_peer_packet_received(const ox::Peer& peer, const ox::Packet& packet) -> void override {
      ox::usize offset = 0;
      auto packet_string = packet.read_string(offset);
      auto packet_u32 = packet.read<ox::u32>(offset);

      ASSERT_TRUE(packet_string.has_value());
      ASSERT_TRUE(packet_u32.has_value());
      EXPECT_EQ(*packet_string, packet_string_value_client);
      EXPECT_EQ(*packet_u32, packet_u32_value);

      OX_LOG_INFO("{}:{}", *packet_string, *packet_u32);

      packet_received = true;
    }

    bool packet_received = false;
  };

  auto server_event_handler = std::make_shared<SEH>();

  const ox::u16 test_port = 7777;
  ox::Server server = {};
  auto
    server_start_result = server.set_event_handler(server_event_handler).set_port(test_port).set_max_clients(2).start();

  ASSERT_TRUE(server_start_result.has_value());

  ox::Client client = {};
  auto connect_result = client.set_connect_timeout(1000).set_disconnect_timeout(1000).connect_async(
    "127.0.0.1",
    test_port
  );

  ASSERT_TRUE(connect_result.has_value());

  // Connect
  bool connected = update_until(client, server, [&]() { return client.is_connected(); });
  ASSERT_TRUE(connected);

  // Send packet
  auto packet_send_result = client.send_packet(*test_packet_client);
  ASSERT_TRUE(packet_send_result.has_value());

  // Update until packet is received
  bool received = update_until(
    client,
    server,
    [&]() { return server_event_handler->packet_received; },
    std::chrono::milliseconds(500)
  );

  EXPECT_TRUE(received) << "Server did not receive packet";

  // Cleanup
  auto disconnect_result = client.disconnect();
  EXPECT_TRUE(disconnect_result.has_value());

  update_until(client, server, [&]() { return server.get_peer_count() == 0; }, std::chrono::milliseconds(500));

  auto server_stop_result = server.stop();
  EXPECT_TRUE(server_stop_result.has_value());
}

TEST_F(NetworkingTest, TestClientReceivePacket) {
  struct CEH : public ox::ClientEventHandler {
    auto on_packet_received(const ox::Packet& packet) -> void override {
      ox::usize offset = 0;
      auto packet_string = packet.read_string(offset);
      auto packet_u32 = packet.read<ox::u32>(offset);

      ASSERT_TRUE(packet_string.has_value());
      ASSERT_TRUE(packet_u32.has_value());
      EXPECT_EQ(*packet_string, packet_string_value_server);
      EXPECT_EQ(*packet_u32, packet_u32_value);

      OX_LOG_INFO("{}:{}", *packet_string, *packet_u32);

      packet_received = true;
    }

    bool packet_received = false;
  };

  const ox::u16 test_port = 7777;
  ox::Server server = {};
  auto server_start_result = server.set_port(test_port).set_max_clients(2).start();

  ASSERT_TRUE(server_start_result.has_value());

  auto client_event_handler = std::make_shared<CEH>();

  ox::Client client = {};
  auto connect_result = client.set_connect_timeout(1000)
                          .set_disconnect_timeout(1000)
                          .set_event_handler(client_event_handler)
                          .connect_async("127.0.0.1", test_port);

  ASSERT_TRUE(connect_result.has_value());

  // Connect
  bool connected = update_until(client, server, [&]() { return client.is_connected() && server.get_peer_count() > 0; });
  ASSERT_TRUE(connected);

  // Get peer and send packet from server
  const auto& client_peer = server.get_peer(client);
  auto packet_send_result = server.send_packet(client_peer, *test_packet_server);
  ASSERT_TRUE(packet_send_result.has_value());

  bool received = update_until(
    client,
    server,
    [&]() { return client_event_handler->packet_received; },
    std::chrono::milliseconds(500)
  );

  EXPECT_TRUE(received) << "Client did not receive packet";

  auto disconnect_result = client.disconnect();
  EXPECT_TRUE(disconnect_result.has_value());

  update_until(client, server, [&]() { return server.get_peer_count() == 0; }, std::chrono::milliseconds(500));

  auto server_stop_result = server.stop();
  EXPECT_TRUE(server_stop_result.has_value());
}

TEST_F(NetworkingTest, TestBlockingConnect) {
  const ox::u16 test_port = 7777;
  ox::Server server = {};
  auto server_start_result = server.set_port(test_port).set_max_clients(2).start();

  ASSERT_TRUE(server_start_result.has_value());

  std::atomic<bool> should_update{true};
  std::thread server_thread([&]() {
    while (should_update) {
      server.update();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  ox::Client client = {};
  auto connect_result = client.set_connect_timeout(1000).connect("127.0.0.1", test_port);

  EXPECT_TRUE(connect_result.has_value()) << connect_result.error();
  EXPECT_TRUE(client.is_connected());

  auto disconnect_result = client.disconnect();
  EXPECT_TRUE(disconnect_result.has_value());

  should_update = false;
  server_thread.join();

  auto server_stop_result = server.stop();
  if (!server_stop_result.has_value()) {
    OX_LOG_ERROR("{}", server_stop_result.error());
  }
  EXPECT_TRUE(server_stop_result.has_value());
}
