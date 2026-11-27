#include <gtest/gtest.h>
#include <vector>

#include "UI/RmlRenderPlan.hpp"

using namespace ox;

class RmlRenderPlanTest : public ::testing::Test {
protected:
  void SetUp() override { builder.begin({800, 600}); }

  auto signature() -> std::vector<u32> {
    auto out = std::vector<u32>{};
    builder.plan.signature(out);
    return out;
  }

  RmlPlanBuilder builder = {};
};

TEST_F(RmlRenderPlanTest, MergesDrawsIntoOnePassAndClampsTheScissor) {
  builder.draw({.index_count = 3});
  builder.set_scissor_enabled(true);
  builder.set_scissor({-10, 590, 100, 100});
  builder.draw({.index_count = 6});

  const auto& plan = builder.plan;
  ASSERT_EQ(plan.passes.size(), 1u);
  EXPECT_EQ(plan.passes[0].kind, RmlPassKind::Draw);
  EXPECT_EQ(plan.passes[0].draw_count, 2u);
  EXPECT_EQ(plan.draws[0].scissor, glm::ivec4(0, 0, 800, 600));
  EXPECT_EQ(plan.draws[1].scissor, glm::ivec4(0, 590, 90, 10));
}

TEST_F(RmlRenderPlanTest, ClipMasksSetTheReferenceDrawsTestAgainst) {
  builder.set_clip_mask_enabled(true);
  builder.clip(Rml::ClipMaskOperation::Set, {});
  builder.draw({});
  builder.clip(Rml::ClipMaskOperation::Intersect, {});
  builder.draw({});
  builder.set_clip_mask_enabled(false);
  builder.draw({});
  builder.set_clip_mask_enabled(true);
  builder.clip(Rml::ClipMaskOperation::SetInverse, {});
  builder.draw({});

  const auto& passes = builder.plan.passes;
  ASSERT_EQ(passes.size(), 9u);
  EXPECT_TRUE(builder.plan.uses_stencil);

  EXPECT_EQ(passes[0].kind, RmlPassKind::ClipClear);
  EXPECT_EQ(passes[0].stencil, 0);
  EXPECT_EQ(passes[1].kind, RmlPassKind::ClipDraw);
  EXPECT_EQ(passes[1].clip_write, RmlClipWrite::Replace);
  EXPECT_EQ(passes[1].stencil, 1);
  EXPECT_EQ(passes[2].kind, RmlPassKind::Draw);
  EXPECT_EQ(passes[2].stencil, 1);

  EXPECT_EQ(passes[3].clip_write, RmlClipWrite::Increment);
  EXPECT_EQ(passes[3].stencil, 1);
  EXPECT_EQ(passes[4].stencil, 2);

  EXPECT_EQ(passes[5].kind, RmlPassKind::Draw);
  EXPECT_EQ(passes[5].stencil, 0);

  EXPECT_EQ(passes[6].kind, RmlPassKind::ClipClear);
  EXPECT_EQ(passes[6].stencil, 1);
  EXPECT_EQ(passes[7].clip_write, RmlClipWrite::Replace);
  EXPECT_EQ(passes[7].stencil, 0);
  EXPECT_EQ(passes[8].stencil, 1);
}

TEST_F(RmlRenderPlanTest, LayersTakeTheDrawsUntilTheyArePopped) {
  builder.draw({});
  const auto layer = builder.push_layer();
  builder.draw({});
  const auto filters = std::vector{RmlFilter{.kind = RmlFilterKind::Blur, .value = 4.0f}};
  builder.composite(layer, RmlLayerID::Base, Rml::BlendMode::Blend, filters);
  builder.pop_layer();
  builder.draw({});

  const auto& plan = builder.plan;
  EXPECT_EQ(layer, RmlLayerID{1});
  EXPECT_EQ(plan.layer_count, 2);
  ASSERT_EQ(plan.passes.size(), 5u);
  EXPECT_EQ(plan.passes[1].kind, RmlPassKind::PushLayer);
  EXPECT_EQ(plan.passes[2].layer, layer);
  EXPECT_EQ(plan.passes[3].kind, RmlPassKind::Composite);
  EXPECT_EQ(plan.passes[3].source, layer);
  EXPECT_EQ(plan.passes[3].layer, RmlLayerID::Base);
  EXPECT_EQ(plan.passes[3].filter_count, 1u);
  EXPECT_EQ(plan.passes[4].layer, RmlLayerID::Base);

  // a layer that doesn't exist is dropped rather than read
  builder.composite(RmlLayerID{7}, RmlLayerID::Base, Rml::BlendMode::Blend, {});
  EXPECT_EQ(plan.passes.size(), 5u);
}

TEST_F(RmlRenderPlanTest, DrawsDeclareTheTexturesSavedThisFrame) {
  constexpr auto saved_handle = Rml::TextureHandle{42};
  builder.push_layer();
  builder.draw({});
  const auto slot = builder.save_texture(saved_handle);
  builder.pop_layer();
  builder.draw({.texture = Rml::TextureHandle{5}});
  builder.draw({.texture = saved_handle});
  builder.draw({.texture = saved_handle});

  const auto& plan = builder.plan;
  const auto& last = plan.passes.back();
  EXPECT_EQ(plan.passes[2].kind, RmlPassKind::SaveTexture);
  EXPECT_EQ(plan.passes[2].layer, RmlLayerID{1});
  EXPECT_EQ(last.draw_count, 3u);
  ASSERT_EQ(last.read_count, 1u);
  EXPECT_EQ(plan.texture_reads[last.first_read], slot);
}

TEST_F(RmlRenderPlanTest, OnlyTheStructureChangesTheSignature) {
  const auto record = [](RmlPlanBuilder& frame, i32 draws, f32 sigma, glm::ivec4 scissor) -> std::vector<u32> {
    frame.begin({800, 600});
    frame.set_scissor_enabled(true);
    frame.set_scissor(scissor);
    for (i32 i = 0; i < draws; i++) {
      frame.draw({.index_count = static_cast<u32>(i + 3)});
    }

    const auto layer = frame.push_layer();
    frame.draw({});
    const auto filters = std::vector{RmlFilter{.kind = RmlFilterKind::DropShadow, .value = sigma}};
    frame.composite(layer, RmlLayerID::Base, Rml::BlendMode::Blend, filters);
    frame.pop_layer();

    auto out = std::vector<u32>{};
    frame.plan.signature(out);
    return out;
  };

  auto other = RmlPlanBuilder{};
  const auto reference = record(builder, 2, 3.0f, {0, 0, 100, 100});
  EXPECT_EQ(record(other, 5, 6.0f, {10, 20, 30, 40}), reference);
  // under half a pixel the shadow isn't blurred, which drops passes
  EXPECT_NE(record(other, 2, 0.25f, {0, 0, 100, 100}), reference);

  other.begin({800, 601});
  auto resized = std::vector<u32>{};
  other.plan.signature(resized);
  builder.begin({800, 600});
  EXPECT_NE(resized, signature());
}
