#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Asset/AssetFile.hpp"
#include "Utils/Log.hpp"

using namespace ox;

class AssetFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    loguru::g_stderr_verbosity = loguru::Verbosity_WARNING;

    directory = std::filesystem::temp_directory_path() / "ox_asset_file_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
  }

  void TearDown() override { std::filesystem::remove_all(directory); }

  static auto make_model() -> ModelData {
    auto model = ModelData{};
    model.name = "tree";

    auto mesh = ModelData::Mesh{};
    mesh.name = "trunk";
    mesh.vertex_positions_offset = 64;
    mesh.vertex_count = 1234;
    mesh.lod_count = 3;
    mesh.lod_metadata_offset = 4096;
    mesh.has_texture_coords = true;
    mesh.bounds_center[1] = 2.5f;
    mesh.bounds_extent[2] = 7.25f;
    mesh.lods[2].indices = 777;
    mesh.lods[2].meshlet_count = 42;
    mesh.lods[2].error = 0.125f;
    mesh.blob = {1, 2, 3, 4, 5};
    mesh.collision_positions = {1.0f, 2.0f, 3.0f};
    mesh.collision_indices = {0, 1, 2};
    mesh.material_index = 0;
    model.meshes.push_back(std::move(mesh));

    auto material = ModelData::Material{};
    material.name = "bark";
    material.uuid.bytes = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
    material.alpha_mode = AlphaMode::Mask;
    material.sampling_mode = SamplingMode::NearestClamped;
    material.albedo_texture_index = 0;
    model.materials.push_back(std::move(material));

    model.textures.push_back({.name = "bark_albedo", .uuid = {.bytes = {4, 2}}, .is_srgb = true});
    model.lights.push_back(
      {.name = "sun", .type = ModelLightType::Spot, .intensity = 4.0f, .has_range = true, .range = 9.0f}
    );
    model.mesh_groups.push_back({.name = "root", .child_indices = {1, 2}, .mesh_indices = {0}});

    return model;
  }

  static auto make_texture() -> TextureData {
    auto texture = TextureData{};
    texture.name = "bark_albedo";
    texture.vk_format = 145;
    texture.width = 256;
    texture.height = 128;
    texture.mips.push_back({.width = 256, .height = 128, .pixels = {0xAA, 0xBB}});
    return texture;
  }

  std::filesystem::path directory = {};
};

// If this fails, zpp_bits ignored AssetFileEntry::serialize and fell back to reflecting members,
// which silently makes the variant index the wire format instead of the AssetType tag.
static_assert(zpp::bits::concepts::has_explicit_serialize<AssetFileEntry>);

TEST_F(AssetFileTest, PacksAndUnpacksEveryPayloadType) {
  auto file = AssetFile{};
  file.add_entry(make_model(), PackedUUID{.bytes = {1, 1, 2, 3, 5, 8}});
  file.add_entry(make_texture(), PackedUUID{.bytes = {7, 7, 7}});

  auto shader = ShaderPipelineData{.module_name = "sky_view"};
  shader.entry_points.push_back({.name = "cs_main", .shader_stage = ShaderStage::Compute, .spirv = {1, 2, 3}});
  file.add_entry(std::move(shader));

  const auto path = directory / "test.oxpack";
  ASSERT_TRUE(file.pack(path));

  auto read = AssetFile::unpack(path);
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(read->entries.size(), 3u);

  EXPECT_EQ(read->entries[0].type, AssetType::Model);
  EXPECT_EQ(read->entries[0].uuid.bytes[4], 5);
  const auto* model = std::get_if<ModelData>(&read->entries[0].data);
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->name, "tree");
  ASSERT_EQ(model->meshes.size(), 1u);
  EXPECT_EQ(model->meshes[0].name, "trunk");
  EXPECT_EQ(model->meshes[0].vertex_count, 1234u);
  EXPECT_EQ(model->meshes[0].lod_metadata_offset, 4096u);
  EXPECT_TRUE(model->meshes[0].has_texture_coords);
  EXPECT_FLOAT_EQ(model->meshes[0].bounds_center[1], 2.5f);
  EXPECT_FLOAT_EQ(model->meshes[0].bounds_extent[2], 7.25f);
  EXPECT_EQ(model->meshes[0].lods.size(), GPU::Mesh::MAX_LODS);
  EXPECT_EQ(model->meshes[0].lods[2].indices, 777u);
  EXPECT_EQ(model->meshes[0].lods[2].meshlet_count, 42u);
  EXPECT_FLOAT_EQ(model->meshes[0].lods[2].error, 0.125f);
  EXPECT_THAT(model->meshes[0].blob, ::testing::ElementsAre(1, 2, 3, 4, 5));
  EXPECT_THAT(model->meshes[0].collision_positions, ::testing::ElementsAre(1.0f, 2.0f, 3.0f));
  EXPECT_THAT(model->meshes[0].collision_indices, ::testing::ElementsAre(0u, 1u, 2u));
  EXPECT_EQ(model->meshes[0].material_index, 0u);
  ASSERT_EQ(model->materials.size(), 1u);
  EXPECT_EQ(model->materials[0].name, "bark");
  EXPECT_EQ(model->materials[0].uuid.bytes[0], 9);
  EXPECT_EQ(model->materials[0].alpha_mode, AlphaMode::Mask);
  EXPECT_EQ(model->materials[0].sampling_mode, SamplingMode::NearestClamped);
  EXPECT_EQ(model->materials[0].albedo_texture_index, 0u);
  // an index that was never assigned must survive as the sentinel, not as 0
  EXPECT_EQ(model->materials[0].normal_texture_index, ModelData::INVALID_INDEX);
  ASSERT_EQ(model->textures.size(), 1u);
  EXPECT_EQ(model->textures[0].uuid.bytes[0], 4);
  ASSERT_EQ(model->lights.size(), 1u);
  EXPECT_EQ(model->lights[0].type, ModelLightType::Spot);
  EXPECT_TRUE(model->lights[0].has_range);
  EXPECT_FLOAT_EQ(model->lights[0].range, 9.0f);
  ASSERT_EQ(model->mesh_groups.size(), 1u);
  EXPECT_THAT(model->mesh_groups[0].child_indices, ::testing::ElementsAre(1u, 2u));

  EXPECT_EQ(read->entries[1].type, AssetType::Texture);
  const auto* texture = std::get_if<TextureData>(&read->entries[1].data);
  ASSERT_NE(texture, nullptr);
  EXPECT_EQ(texture->name, "bark_albedo");
  EXPECT_EQ(texture->vk_format, 145u);
  EXPECT_EQ(texture->width, 256u);
  EXPECT_EQ(texture->height, 128u);
  ASSERT_EQ(texture->mips.size(), 1u);
  EXPECT_THAT(texture->mips[0].pixels, ::testing::ElementsAre(0xAA, 0xBB));

  EXPECT_EQ(read->entries[2].type, AssetType::Shader);
  const auto* pipeline = std::get_if<ShaderPipelineData>(&read->entries[2].data);
  ASSERT_NE(pipeline, nullptr);
  EXPECT_EQ(pipeline->module_name, "sky_view");
  ASSERT_EQ(pipeline->entry_points.size(), 1u);
  EXPECT_EQ(pipeline->entry_points[0].shader_stage, ShaderStage::Compute);
  EXPECT_THAT(pipeline->entry_points[0].spirv, ::testing::ElementsAre(1u, 2u, 3u));
}

TEST_F(AssetFileTest, RejectsAPackFromAnOtherVersion) {
  auto file = AssetFile{};
  file.add_entry(make_texture());

  const auto path = directory / "stale.oxpack";
  ASSERT_TRUE(file.pack(path));
  ASSERT_TRUE(AssetFile::unpack(path).has_value());

  auto bytes = std::vector<u8>{};
  {
    auto in = std::ifstream(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  ASSERT_GT(bytes.size(), sizeof(u32) + sizeof(u16));
  // the version follows the 4-byte magic
  bytes[sizeof(u32)] = static_cast<u8>(AssetFileHeader::VERSION + 1);
  {
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const c8*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  EXPECT_FALSE(AssetFile::unpack(path).has_value());
}

TEST_F(AssetFileTest, RejectsAPackWithABadSignature) {
  auto file = AssetFile{};
  file.add_entry(make_texture());

  const auto path = directory / "corrupt.oxpack";
  ASSERT_TRUE(file.pack(path));

  auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
  const auto garbage = std::array<u8, 32>{};
  out.write(reinterpret_cast<const c8*>(garbage.data()), garbage.size());
  out.close();

  EXPECT_FALSE(AssetFile::unpack(path).has_value());
}

TEST_F(AssetFileTest, RoundTripsAPackedUUID) {
  const auto uuid = UUID::generate_random();
  EXPECT_EQ(PackedUUID::pack(uuid).unpack(), uuid);
}
