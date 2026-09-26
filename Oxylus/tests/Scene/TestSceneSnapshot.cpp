#include <gtest/gtest.h>

#include "Scene/SceneSnapshot.hpp"

static auto advance_n(ox::SceneSnapshotBuilder& builder, u32 count) -> void {
  for (auto i = 0_u32; i < count; i++) {
    builder.advance();
  }
}

TEST(SceneSnapshotBuilderTest, NothingAckedFindsNothing) {
  auto builder = ox::SceneSnapshotBuilder{};
  advance_n(builder, 5);

  EXPECT_FALSE(builder.find_last_acked().has_value());
}

TEST(SceneSnapshotBuilderTest, FindsAnOlderAck) {
  auto builder = ox::SceneSnapshotBuilder{};
  advance_n(builder, 3);
  builder.ack(0);

  auto last_acked = builder.find_last_acked();
  ASSERT_TRUE(last_acked.has_value());
  EXPECT_EQ(last_acked.value(), 0_u8);
}

TEST(SceneSnapshotBuilderTest, PrefersTheNewestAck) {
  auto builder = ox::SceneSnapshotBuilder{};
  advance_n(builder, 5);
  builder.ack(1);
  builder.ack(3);

  auto last_acked = builder.find_last_acked();
  ASSERT_TRUE(last_acked.has_value());
  EXPECT_EQ(last_acked.value(), 3_u8);
}

TEST(SceneSnapshotBuilderTest, WalksBackAcrossTheWrap) {
  auto builder = ox::SceneSnapshotBuilder{};
  advance_n(builder, ox::SceneSnapshotBuilder::MAX_SEQUENCES + 1);
  ASSERT_EQ(builder.current_sequence, 1_u8);
  builder.ack(ox::SceneSnapshotBuilder::MAX_SEQUENCES - 1);

  auto last_acked = builder.find_last_acked();
  ASSERT_TRUE(last_acked.has_value());
  EXPECT_EQ(last_acked.value(), ox::SceneSnapshotBuilder::MAX_SEQUENCES - 1);
}

TEST(SceneSnapshotBuilderTest, IgnoresTheCurrentSequence) {
  auto builder = ox::SceneSnapshotBuilder{};
  advance_n(builder, 2);
  builder.ack(2);

  EXPECT_FALSE(builder.find_last_acked().has_value());
}

TEST(SceneSnapshotBuilderTest, AdvancingOntoASlotClearsItsAck) {
  auto builder = ox::SceneSnapshotBuilder{};
  builder.ack(1);
  ASSERT_TRUE(builder.find_last_acked().has_value());

  builder.advance();

  EXPECT_FALSE(builder.find_last_acked().has_value());
}
