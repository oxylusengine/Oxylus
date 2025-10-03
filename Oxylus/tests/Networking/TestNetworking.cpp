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

    this->test_packet = std::make_unique<ox::Packet>(packet_id);
    this->test_packet->add_string(packet_string_value).add<ox::u32>(packet_u32_value);
  }

  void TearDown() override {
    auto _ = network_manager->deinit();
    log_test_end();
  }

  std::unique_ptr<ox::NetworkManager> network_manager = nullptr;
  std::unique_ptr<ox::Packet> test_packet = nullptr;
  static constexpr auto packet_string_value = "Hi from oxylus client!";
  static constexpr ox::u32 packet_u32_value = 31;
  static constexpr ox::u32 packet_id = 0;
};

TEST_F(NetworkingTest, TestServerClient) {
  const ox::u16 test_port = 7777;
  ox::Server server = {};
  auto server_start_result = server //
                               .set_port(test_port)
                               .set_max_clients(2)
                               .start();

  if (!server_start_result.has_value()) {
    OX_LOG_ERROR("{}", server_start_result.error());
  }
  EXPECT_TRUE(server_start_result.has_value());

  EXPECT_TRUE(server.is_running());

  ox::Client client = {};
  auto client_connection_request_result = client.set_connect_timeout(1000)
                                            .set_disconnect_timeout(1000)
                                            .request_connection("127.0.0.1", test_port);
  if (!client_connection_request_result.has_value()) {
    OX_LOG_ERROR("{}", client_connection_request_result.error());
  }
  EXPECT_TRUE(client_connection_request_result.has_value());

  client.update();
  server.update();

  auto client_connection_result = client.wait_for_connection();
  if (!client_connection_result.has_value()) {
    OX_LOG_ERROR("{}", client_connection_result.error());
  }
  EXPECT_TRUE(client_connection_result.has_value());

  EXPECT_TRUE(client.is_connected());

  client.update();
  server.update();

  EXPECT_EQ(server.get_peer_count(), 1);

  auto _ = client.disconnect();
  EXPECT_FALSE(client.is_connected());

  client.update();
  server.update();

  EXPECT_EQ(server.get_peer_count(), 0);

  auto server_stop_result = server.stop();
  if (!server_stop_result.has_value()) {
    OX_LOG_ERROR("{}", server_stop_result.error());
  }
  EXPECT_TRUE(server_stop_result.has_value());
}

TEST_F(NetworkingTest, TestServerRecievePacket) {
  struct SEH : public ox::ServerEventHandler {
    auto on_peer_packet_received(const ox::Peer& peer, const ox::Packet& packet) -> void override {
      ox::usize offset = 0;
      auto packet_string = packet.read_string(offset);
      auto packet_u32 = packet.read<ox::u32>(offset);
      if (packet_string.has_value() && packet_u32.has_value()) {
        EXPECT_TRUE(*packet_string == packet_string_value);
        EXPECT_TRUE(*packet_u32 == packet_u32_value);
        OX_LOG_INFO("{}:{}", *packet_string, *packet_u32);
      }
      EXPECT_TRUE(packet_string.has_value());
      EXPECT_TRUE(packet_u32.has_value());
    }
  };

  auto server_event_handler = std::make_shared<SEH>();

  const ox::u16 test_port = 7777;
  ox::Server server = {};
  auto server_start_result = server //
                               .set_event_handler(server_event_handler)
                               .set_port(test_port)
                               .set_max_clients(2)
                               .start();

  if (!server_start_result.has_value()) {
    OX_LOG_ERROR("{}", server_start_result.error());
  }
  EXPECT_TRUE(server_start_result.has_value());

  EXPECT_TRUE(server.is_running());

  ox::Client client = {};
  auto client_connection_request_result = client.set_connect_timeout(1000)
                                            .set_disconnect_timeout(1000)
                                            .request_connection("127.0.0.1", test_port);
  if (!client_connection_request_result.has_value()) {
    OX_LOG_ERROR("{}", client_connection_request_result.error());
  }
  EXPECT_TRUE(client_connection_request_result.has_value());

  client.update();
  server.update();

  auto client_connection_result = client.wait_for_connection();
  if (!client_connection_result.has_value()) {
    OX_LOG_ERROR("{}", client_connection_result.error());
  }
  EXPECT_TRUE(client_connection_result.has_value());

  EXPECT_TRUE(client.is_connected());

  client.update();
  server.update();

  auto packet_send_result = client.send_packet(*test_packet);
  if (!packet_send_result.has_value()) {
    OX_LOG_ERROR("{}", packet_send_result.error());
  }
  EXPECT_TRUE(packet_send_result.has_value());

  client.update();
  server.update();

  auto _ = client.disconnect();
  EXPECT_FALSE(client.is_connected());

  client.update();
  server.update();

  EXPECT_EQ(server.get_peer_count(), 0);

  auto server_stop_result = server.stop();
  if (!server_stop_result.has_value()) {
    OX_LOG_ERROR("{}", server_stop_result.error());
  }
  EXPECT_TRUE(server_stop_result.has_value());
}

TEST_F(NetworkingTest, TestClientRecievePacket) {
  struct CEH : public ox::ClientEventHandler {
    virtual auto on_packet_received(const ox::Packet& packet) -> void override {
      ox::usize offset = 0;
      auto packet_string = packet.read_string(offset);
      auto packet_u32 = packet.read<ox::u32>(offset);
      if (packet_string.has_value() && packet_u32.has_value()) {
        EXPECT_TRUE(*packet_string == packet_string_value);
        EXPECT_TRUE(*packet_u32 == packet_u32_value);
        OX_LOG_INFO("{}:{}", *packet_string, *packet_u32);
      }
      EXPECT_TRUE(packet_string.has_value());
      EXPECT_TRUE(packet_u32.has_value());
    }
  };

  const ox::u16 test_port = 7777;
  ox::Server server = {};
  auto server_start_result = server //
                               .set_port(test_port)
                               .set_max_clients(2)
                               .start();

  if (!server_start_result.has_value()) {
    OX_LOG_ERROR("{}", server_start_result.error());
  }
  EXPECT_TRUE(server_start_result.has_value());

  EXPECT_TRUE(server.is_running());

  auto client_event_handler = std::make_shared<CEH>();

  ox::Client client = {};
  auto client_connection_request_result = client.set_connect_timeout(1000)
                                            .set_disconnect_timeout(1000)
                                            .set_event_handler(client_event_handler)
                                            .request_connection("127.0.0.1", test_port);
  if (!client_connection_request_result.has_value()) {
    OX_LOG_ERROR("{}", client_connection_request_result.error());
  }
  EXPECT_TRUE(client_connection_request_result.has_value());

  client.update();
  server.update();

  auto client_connection_result = client.wait_for_connection();
  if (!client_connection_result.has_value()) {
    OX_LOG_ERROR("{}", client_connection_result.error());
  }
  EXPECT_TRUE(client_connection_result.has_value());

  EXPECT_TRUE(client.is_connected());

  client.update();
  server.update();

  const auto& client_peer = server.get_peer(client);

  auto packet_send_result = server.send_packet(client_peer, *test_packet);
  if (!packet_send_result.has_value()) {
    OX_LOG_ERROR("{}", packet_send_result.error());
  }
  EXPECT_TRUE(packet_send_result.has_value());

  server.update();
  client.update();

  auto _ = client.disconnect();
  EXPECT_FALSE(client.is_connected());

  client.update();
  server.update();

  EXPECT_EQ(server.get_peer_count(), 0);

  auto server_stop_result = server.stop();
  if (!server_stop_result.has_value()) {
    OX_LOG_ERROR("{}", server_stop_result.error());
  }
  EXPECT_TRUE(server_stop_result.has_value());
}
