#include <gtest/gtest.h>
#include <limits>

#include "UI/UIScale.hpp"

using namespace ox;

TEST(UIScale, NormalizesUserMultiplier) {
  EXPECT_FLOAT_EQ(normalize_ui_scale_multiplier(0.1f), UI_SCALE_MIN_MULTIPLIER);
  EXPECT_FLOAT_EQ(normalize_ui_scale_multiplier(4.0f), UI_SCALE_MAX_MULTIPLIER);
  EXPECT_NEAR(normalize_ui_scale_multiplier(1.13f), 1.15f, 0.0001f);
  EXPECT_FLOAT_EQ(normalize_ui_scale_multiplier(std::numeric_limits<f32>::quiet_NaN()), UI_SCALE_DEFAULT_MULTIPLIER);
  EXPECT_FLOAT_EQ(normalize_ui_scale_multiplier(std::numeric_limits<f32>::infinity()), UI_SCALE_DEFAULT_MULTIPLIER);
}

TEST(UIScale, CombinesDisplayScaleAndUserMultiplier) {
  EXPECT_NEAR(calculate_ui_scale(1.5f, 1.13f), 1.725f, 0.0001f);
  EXPECT_FLOAT_EQ(calculate_ui_scale(0.0f, 1.5f), 1.5f);
  EXPECT_FLOAT_EQ(calculate_ui_scale(std::numeric_limits<f32>::quiet_NaN(), 1.0f), 1.0f);
}

TEST(UIScale, CompensatesRmlUiForRenderSurfaceScaling) {
  EXPECT_FLOAT_EQ(calculate_rml_dpi_ratio(1.5f, 800.0f, 1200), 2.25f);
  EXPECT_FLOAT_EQ(calculate_rml_dpi_ratio(1.5f, 800.0f, 400), 0.75f);
  EXPECT_FLOAT_EQ(calculate_rml_dpi_ratio(1.5f, 0.0f, 1200), 1.5f);
  EXPECT_FLOAT_EQ(calculate_rml_dpi_ratio(1.5f, 800.0f, 0), 1.5f);
}
