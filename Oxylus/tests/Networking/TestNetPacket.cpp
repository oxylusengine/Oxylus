#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <array>
#include <cstring>
#include <enet.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <vector>

#include "Core/Base.hpp"
#include "Core/UUID.hpp"
#include "Networking/NetPacket.hpp"

class NetPacketTest : public ::testing::Test {
protected:
  static auto SetUpTestSuite() -> void { ASSERT_EQ(enet_initialize(), 0); }
  static auto TearDownTestSuite() -> void { enet_deinitialize(); }

  // Wraps the sent packet the same way the receiving side does. The returned packet shares `inner`
  // with the sent one, so only the sent one is ever destroyed.
  static auto receive(ox::NetPacket& sent) -> ox::option<ox::NetPacket> {
    return ox::NetPacket::from_packet(sent.inner);
  }

  static auto payload_of(ox::NetPacket& packet) -> std::vector<u8> {
    return {packet.inner->data, packet.inner->data + packet.inner->dataLength};
  }

  static auto make_test_state() -> ox::SceneState {
    auto state = ox::SceneState{};

    auto entity = ox::EntityState{.entity_id = 42};
    entity.components.emplace(7, ox::ComponentState{.id = 7, .hash = 0xabcdef_u64, .buffer = {1, 2, 3, 4, 5}});
    // A tag, no data attached to it.
    entity.components.emplace(9, ox::ComponentState{.id = 9, .hash = ~0_u64, .buffer = {}});
    entity.removed_components.emplace(11);
    state.entities.emplace(42, std::move(entity));

    // An entity that only exists, without any component changes.
    state.entities.emplace(43, ox::EntityState{.entity_id = 43});
    state.removed_entities.emplace(1337);

    return state;
  }
};

TEST_F(NetPacketTest, HandshakeRoundTrip) {
  auto sent = ox::NetPacket::handshake({.version = 7, .net_id = 0xdeadbeefcafe_u64});
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  EXPECT_EQ(sent->type, ox::NetPacketType::Handshake);

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->type, ox::NetPacketType::Handshake);

  auto info = received->get_handshake();
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->version, 7_u32);
  EXPECT_EQ(info->net_id, 0xdeadbeefcafe_u64);
}

TEST_F(NetPacketTest, ClientAckRoundTrip) {
  auto sent = ox::NetPacket::client_ack({.acked = 17});
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  auto info = received->get_client_ack();
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->acked, 17_u8);
}

TEST_F(NetPacketTest, ClientAckRoundTripsEveryValue) {
  // 255 used to collide with the `option<u8>` sentinel and got rejected as a malformed packet.
  for (auto acked = 0_u32; acked <= 255_u32; acked++) {
    auto sent = ox::NetPacket::client_ack({.acked = static_cast<u8>(acked)});
    ASSERT_TRUE(sent.has_value());
    OX_DEFER(&) { sent->destroy(); };

    auto received = receive(sent.value());
    ASSERT_TRUE(received.has_value());

    auto info = received->get_client_ack();
    ASSERT_TRUE(info.has_value()) << "acked = " << acked;
    EXPECT_EQ(info->acked, static_cast<u8>(acked));
  }
}

TEST_F(NetPacketTest, SceneSnapshotRoundTrip) {
  const auto state = make_test_state();

  auto sent = ox::NetPacket::scene_snapshot(state, 3);
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  EXPECT_EQ(sent->type, ox::NetPacketType::SceneSnapshot);

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  auto snapshot = received->get_scene_snapshot();
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->sequence, 3_u8);

  const auto& result = snapshot->state;
  ASSERT_EQ(result.entities.size(), 2_sz);
  ASSERT_TRUE(result.entities.contains(42));
  ASSERT_TRUE(result.entities.contains(43));
  EXPECT_THAT(result.removed_entities, testing::UnorderedElementsAre(1337));

  const auto& entity = result.entities.at(42);
  EXPECT_EQ(entity.entity_id, 42_u64);
  ASSERT_EQ(entity.components.size(), 2_sz);
  EXPECT_THAT(entity.removed_components, testing::UnorderedElementsAre(11));

  const auto& component = entity.components.at(7);
  EXPECT_EQ(component.id, 7_u64);
  EXPECT_EQ(component.hash, 0xabcdef_u64);
  EXPECT_THAT(component.buffer, testing::ElementsAre(1, 2, 3, 4, 5));

  const auto& tag = entity.components.at(9);
  EXPECT_EQ(tag.id, 9_u64);
  EXPECT_EQ(tag.hash, ~0_u64);
  EXPECT_TRUE(tag.buffer.empty());

  const auto& empty_entity = result.entities.at(43);
  EXPECT_EQ(empty_entity.entity_id, 43_u64);
  EXPECT_TRUE(empty_entity.components.empty());
  EXPECT_TRUE(empty_entity.removed_components.empty());
}

TEST_F(NetPacketTest, SceneSnapshotRoundTripsEmptyState) {
  const auto state = ox::SceneState{};

  auto sent = ox::NetPacket::scene_snapshot(state, 0);
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  auto snapshot = received->get_scene_snapshot();
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->sequence, 0_u8);
  EXPECT_TRUE(snapshot->state.entities.empty());
  EXPECT_TRUE(snapshot->state.removed_entities.empty());
}

TEST_F(NetPacketTest, SceneSnapshotRoundTripsEverySequence) {
  // Same sentinel trap as the client ack, the sequence wraps around all 8 bits.
  for (auto sequence = 0_u32; sequence <= 255_u32; sequence++) {
    auto sent = ox::NetPacket::scene_snapshot(ox::SceneState{}, static_cast<u8>(sequence));
    ASSERT_TRUE(sent.has_value());
    OX_DEFER(&) { sent->destroy(); };

    auto received = receive(sent.value());
    ASSERT_TRUE(received.has_value());

    auto snapshot = received->get_scene_snapshot();
    ASSERT_TRUE(snapshot.has_value()) << "sequence = " << sequence;
    EXPECT_EQ(snapshot->sequence, static_cast<u8>(sequence));
  }
}

TEST_F(NetPacketTest, SceneSnapshotRoundTripsComponentBufferLargerThanU16) {
  // The hand rolled format wrote component sizes as u16, anything past 64k silently corrupted.
  constexpr auto buffer_size = 100'000_sz;

  auto buffer = std::vector<u8>(buffer_size);
  for (auto i = 0_sz; i < buffer_size; i++) {
    buffer[i] = static_cast<u8>(i);
  }

  auto state = ox::SceneState{};
  auto entity = ox::EntityState{.entity_id = 1};
  entity.components.emplace(2, ox::ComponentState{.id = 2, .hash = 3, .buffer = buffer});
  state.entities.emplace(1, std::move(entity));

  auto sent = ox::NetPacket::scene_snapshot(state, 1);
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  auto snapshot = received->get_scene_snapshot();
  ASSERT_TRUE(snapshot.has_value());

  const auto& result = snapshot->state.entities.at(1).components.at(2).buffer;
  ASSERT_EQ(result.size(), buffer_size);
  EXPECT_TRUE(std::ranges::equal(result, buffer));
}

TEST_F(NetPacketTest, RPCRoundTripsEveryParameterType) {
  const auto uuid = ox::UUID::generate_random();
  auto uuid_bytes = std::array<u8, 16>{};
  std::ranges::copy(uuid.bytes(), uuid_bytes.begin());

  const auto payload = std::array{0xdeadbeef_u32, 0xcafebabe_u32};
  auto payload_bytes = std::vector<u8>(sizeof(payload));
  std::memcpy(payload_bytes.data(), payload.data(), sizeof(payload));

  const auto params = std::array{
    ox::RPCParameter{.value = std::monostate{}},
    ox::RPCParameter{.value = 200_u8},
    ox::RPCParameter{.value = 40000_u16},
    ox::RPCParameter{.value = -123456_i32},
    ox::RPCParameter{.value = -1234567890123_i64},
    ox::RPCParameter{.value = 1.5f},
    ox::RPCParameter{.value = 2.25},
    ox::RPCParameter{.value = std::string("hello rpc")},
    ox::RPCParameter{.value = uuid_bytes},
    ox::RPCParameter{.value = payload_bytes},
  };

  auto sent = ox::NetPacket::rpc("test_proc"sv, params);
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  EXPECT_EQ(sent->type, ox::NetPacketType::RPC);

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  auto rpc = received->get_rpc();
  ASSERT_TRUE(rpc.has_value());
  ASSERT_EQ(rpc->parameters.size(), params.size());

  const auto& result = rpc->parameters;
  EXPECT_TRUE(std::holds_alternative<std::monostate>(result[0].value));
  EXPECT_EQ(std::get<u8>(result[1].value), 200_u8);
  EXPECT_EQ(std::get<u16>(result[2].value), 40000_u16);
  EXPECT_EQ(std::get<i32>(result[3].value), -123456_i32);
  EXPECT_EQ(std::get<f64>(result[6].value), 2.25);

  auto as_int64 = result[4].as_int64();
  ASSERT_TRUE(as_int64.has_value());
  EXPECT_EQ(*as_int64, -1234567890123_i64);

  auto as_f32 = result[5].as_f32();
  ASSERT_TRUE(as_f32.has_value());
  EXPECT_FLOAT_EQ(*as_f32, 1.5f);

  EXPECT_EQ(result[7].as_str(), "hello rpc"sv);

  auto as_uuid = result[8].as_uuid();
  ASSERT_TRUE(as_uuid.has_value());
  EXPECT_TRUE(*as_uuid == uuid);

  EXPECT_THAT(result[9].as_span<u32>(), testing::ElementsAreArray(payload));
}

TEST_F(NetPacketTest, RPCRoundTripsWithoutParameters) {
  auto sent = ox::NetPacket::rpc("no_params"sv, {});
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  auto rpc = received->get_rpc();
  ASSERT_TRUE(rpc.has_value());
  EXPECT_TRUE(rpc->parameters.empty());
}

TEST_F(NetPacketTest, RPCProcHashMatchesIdentifierHash) {
  constexpr auto identifier = "spawn_player"sv;

  auto sent = ox::NetPacket::rpc(identifier, {});
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  auto rpc = received->get_rpc();
  ASSERT_TRUE(rpc.has_value());

  // This is how NetClient/NetServer::register_proc key their callbacks.
  const auto expected = ankerl::unordered_dense::detail::wyhash::hash(identifier.data(), identifier.size());
  EXPECT_EQ(rpc->proc_hash, expected);
}

TEST_F(NetPacketTest, RPCParameterAccessorsRejectMismatchedTypes) {
  const auto param = ox::RPCParameter{.value = std::string("not a number")};

  EXPECT_FALSE(param.as_f32().has_value());
  EXPECT_FALSE(param.as_int64().has_value());
  EXPECT_FALSE(param.as_uuid().has_value());
  EXPECT_TRUE(param.as_span<u32>().empty());
  EXPECT_EQ(param.as_str(), "not a number"sv);

  const auto none = ox::RPCParameter{};
  EXPECT_FALSE(none.as_f32().has_value());
  EXPECT_TRUE(none.as_str().empty());
}

TEST_F(NetPacketTest, GettersRejectMismatchedPacketTypes) {
  auto sent = ox::NetPacket::handshake({.version = 1});
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  auto received = receive(sent.value());
  ASSERT_TRUE(received.has_value());

  EXPECT_TRUE(received->get_handshake().has_value());
  EXPECT_FALSE(received->get_scene_snapshot().has_value());
  EXPECT_FALSE(received->get_client_ack().has_value());
  EXPECT_FALSE(received->get_rpc().has_value());
}

TEST_F(NetPacketTest, FromPacketRejectsEmptyPayload) {
  auto* raw = enet_packet_create(nullptr, 0, 0);
  ASSERT_NE(raw, nullptr);
  OX_DEFER(&) { enet_packet_destroy(raw); };

  EXPECT_FALSE(ox::NetPacket::from_packet(raw).has_value());
}

TEST_F(NetPacketTest, TruncatedPayloadIsRejected) {
  auto sent = ox::NetPacket::scene_snapshot(make_test_state(), 3);
  ASSERT_TRUE(sent.has_value());
  OX_DEFER(&) { sent->destroy(); };

  const auto bytes = payload_of(sent.value());
  ASSERT_GT(bytes.size(), 4_sz);

  // Every cut of a valid packet must be rejected instead of reading out of bounds.
  for (auto size = 1_sz; size < bytes.size(); size++) {
    auto* raw = enet_packet_create(bytes.data(), size, 0);
    ASSERT_NE(raw, nullptr);
    OX_DEFER(&) { enet_packet_destroy(raw); };

    auto truncated = ox::NetPacket::from_packet(raw);
    ASSERT_TRUE(truncated.has_value());
    EXPECT_EQ(truncated->type, ox::NetPacketType::SceneSnapshot);
    EXPECT_FALSE(truncated->get_scene_snapshot().has_value()) << "size = " << size;
  }
}

TEST_F(NetPacketTest, GarbagePayloadIsRejected) {
  auto bytes = std::array<u8, 64>{};
  bytes.fill(0xff);
  bytes[0] = static_cast<u8>(ox::NetPacketType::RPC);

  auto* raw = enet_packet_create(bytes.data(), bytes.size(), 0);
  ASSERT_NE(raw, nullptr);
  OX_DEFER(&) { enet_packet_destroy(raw); };

  auto packet = ox::NetPacket::from_packet(raw);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->type, ox::NetPacketType::RPC);
  EXPECT_FALSE(packet->get_rpc().has_value());
}

TEST_F(NetPacketTest, ImplausibleContainerSizeIsRejected) {
  // A hand rolled RPC packet claiming a billion parameters. Without an allocation limit on the
  // deserializer this would try to allocate tens of gigabytes before looking at a single parameter.
  auto [data, ser] = zpp::bits::data_out(zpp::bits::options::size_varint{});
  ASSERT_FALSE(zpp::bits::failure(ser(ox::NetPacketType::RPC, 0_u64, zpp::bits::vsize_t{1'000'000'000})));

  auto* raw = enet_packet_create(data.data(), data.size(), 0);
  ASSERT_NE(raw, nullptr);
  OX_DEFER(&) { enet_packet_destroy(raw); };

  auto packet = ox::NetPacket::from_packet(raw);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->type, ox::NetPacketType::RPC);
  EXPECT_FALSE(packet->get_rpc().has_value());
}

TEST_F(NetPacketTest, UnknownPacketTypeIsNotClaimedByAnyGetter) {
  const auto bytes = std::array<u8, 4>{200, 1, 2, 3};

  auto* raw = enet_packet_create(bytes.data(), bytes.size(), 0);
  ASSERT_NE(raw, nullptr);
  OX_DEFER(&) { enet_packet_destroy(raw); };

  auto packet = ox::NetPacket::from_packet(raw);
  ASSERT_TRUE(packet.has_value());

  EXPECT_FALSE(packet->get_handshake().has_value());
  EXPECT_FALSE(packet->get_scene_snapshot().has_value());
  EXPECT_FALSE(packet->get_client_ack().has_value());
  EXPECT_FALSE(packet->get_rpc().has_value());
}
