#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <string>

#include "Asset/Material.hpp"
#include "AssetImport.hpp"
#include "ResourceCompiler.hpp"
#include "Utils/JsonWriter.hpp"

using namespace ox;

static auto write_sidecar(const UUID& uuid, const Material& material) -> std::string {
  auto writer = JsonWriter{};
  EXPECT_TRUE(rc::write_material_asset_meta(writer, uuid, material));
  return writer.stream.str();
}

TEST(MaterialSidecar, RoundTripsEveryField) {
  auto material = Material{};
  material.albedo_color = {0.25f, 0.5f, 0.75f, 0.5f};
  material.uv_size = {2.0f, 4.0f};
  material.uv_offset = {0.125f, 0.25f};
  material.emissive_color = {1.0f, 2.0f, 3.0f};
  material.roughness_factor = 0.625f;
  material.metallic_factor = 0.375f;
  material.normal_scale = 0.5f;
  material.occlusion_strength = 0.25f;
  material.alpha_mode = AlphaMode::Mask;
  material.alpha_cutoff = 0.75f;
  material.sampling_mode = SamplingMode::NearestClamped;
  material.flip_normal_y = true;
  material.double_sided = true;
  material.transmission_factor = 0.875f;
  material.ior = 1.25f;
  material.thickness_factor = 2.5f;
  material.attenuation_color = {0.5f, 0.25f, 0.125f};
  material.attenuation_distance = 8.0f;
  material.shading_model = ShadingModel::Water;
  material.water_scatter_color = {0.0625f, 0.125f, 0.1875f};
  material.water_flow = {0.5f, -0.25f};
  material.water_detail_scale = 0.75f;
  material.water_shore_fade = 1.5f;
  material.albedo_texture = UUID::generate_random();
  material.normal_texture = UUID::generate_random();
  material.emissive_texture = UUID::generate_random();
  material.metallic_roughness_texture = UUID::generate_random();
  material.occlusion_texture = UUID::generate_random();
  material.transmission_texture = UUID::generate_random();
  material.thickness_texture = UUID::generate_random();

  auto session = rc::Session::create();
  ASSERT_TRUE(session.has_value());

  auto read = Material{};
  ASSERT_TRUE(rc::parse_material_asset_meta(*session, write_sidecar(UUID::generate_random(), material), read));

  EXPECT_EQ(read.albedo_color, material.albedo_color);
  EXPECT_EQ(read.uv_size, material.uv_size);
  EXPECT_EQ(read.uv_offset, material.uv_offset);
  EXPECT_EQ(read.emissive_color, material.emissive_color);
  EXPECT_FLOAT_EQ(read.roughness_factor, material.roughness_factor);
  EXPECT_FLOAT_EQ(read.metallic_factor, material.metallic_factor);
  EXPECT_FLOAT_EQ(read.normal_scale, material.normal_scale);
  EXPECT_FLOAT_EQ(read.occlusion_strength, material.occlusion_strength);
  EXPECT_EQ(read.alpha_mode, material.alpha_mode);
  EXPECT_FLOAT_EQ(read.alpha_cutoff, material.alpha_cutoff);
  EXPECT_EQ(read.sampling_mode, material.sampling_mode);
  EXPECT_TRUE(read.flip_normal_y);
  EXPECT_TRUE(read.double_sided);
  EXPECT_FLOAT_EQ(read.transmission_factor, material.transmission_factor);
  EXPECT_FLOAT_EQ(read.ior, material.ior);
  EXPECT_FLOAT_EQ(read.thickness_factor, material.thickness_factor);
  EXPECT_EQ(read.attenuation_color, material.attenuation_color);
  EXPECT_FLOAT_EQ(read.attenuation_distance, material.attenuation_distance);
  EXPECT_EQ(read.shading_model, material.shading_model);
  EXPECT_EQ(read.water_scatter_color, material.water_scatter_color);
  EXPECT_EQ(read.water_flow, material.water_flow);
  EXPECT_FLOAT_EQ(read.water_detail_scale, material.water_detail_scale);
  EXPECT_FLOAT_EQ(read.water_shore_fade, material.water_shore_fade);
  EXPECT_EQ(read.albedo_texture, material.albedo_texture);
  EXPECT_EQ(read.normal_texture, material.normal_texture);
  EXPECT_EQ(read.emissive_texture, material.emissive_texture);
  EXPECT_EQ(read.metallic_roughness_texture, material.metallic_roughness_texture);
  EXPECT_EQ(read.occlusion_texture, material.occlusion_texture);
  EXPECT_EQ(read.transmission_texture, material.transmission_texture);
  EXPECT_EQ(read.thickness_texture, material.thickness_texture);
  EXPECT_TRUE(session->get_errors().empty());
}

TEST(MaterialSidecar, MissingKeysKeepTheDefaults) {
  auto session = rc::Session::create();
  ASSERT_TRUE(session.has_value());

  auto read = Material{};
  ASSERT_TRUE(rc::parse_material_asset_meta(*session, R"({"roughness_factor": 0.5})", read));

  const auto defaults = Material{};
  EXPECT_FLOAT_EQ(read.roughness_factor, 0.5f);
  EXPECT_FALSE(read.double_sided);
  EXPECT_FLOAT_EQ(read.transmission_factor, defaults.transmission_factor);
  EXPECT_FLOAT_EQ(read.ior, defaults.ior);
  EXPECT_FLOAT_EQ(read.thickness_factor, defaults.thickness_factor);
  EXPECT_EQ(read.attenuation_color, defaults.attenuation_color);
  EXPECT_FLOAT_EQ(read.attenuation_distance, defaults.attenuation_distance);
  EXPECT_EQ(read.shading_model, ShadingModel::Standard);
  EXPECT_EQ(read.water_scatter_color, defaults.water_scatter_color);
  EXPECT_EQ(read.water_flow, defaults.water_flow);
  EXPECT_FLOAT_EQ(read.water_detail_scale, defaults.water_detail_scale);
  EXPECT_FLOAT_EQ(read.water_shore_fade, defaults.water_shore_fade);
  EXPECT_FALSE(read.transmission_texture);
  EXPECT_FALSE(read.thickness_texture);
  EXPECT_TRUE(session->get_errors().empty());
}

TEST(MaterialSidecar, OutOfRangeEnumsFallBackAndAreReported) {
  auto session = rc::Session::create();
  ASSERT_TRUE(session.has_value());

  auto read = Material{};
  ASSERT_TRUE(
    rc::parse_material_asset_meta(*session, R"({"shading_model": 7, "alpha_mode": 9, "sampling_mode": 100})", read)
  );

  EXPECT_EQ(read.shading_model, ShadingModel::Standard);
  EXPECT_EQ(read.alpha_mode, AlphaMode::Opaque);
  EXPECT_EQ(read.sampling_mode, SamplingMode::LinearRepeated);
  EXPECT_EQ(session->get_errors().size(), 3u);
}
