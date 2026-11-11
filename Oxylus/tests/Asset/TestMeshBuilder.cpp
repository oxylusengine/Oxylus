#include <array>
#include <gtest/gtest.h>
#include <vector>

#include "Asset/MeshBuilder.hpp"

using namespace ox;

// a closed box of 12 triangles with a vertex per corner per face, the way a flat shaded runtime mesh comes in
static auto box_triangles(glm::vec3 half_extent, std::vector<glm::vec3>& positions, std::vector<glm::vec3>& normals)
  -> std::vector<u32> {
  constexpr auto FACES = std::array{
    glm::vec3(1.0f, 0.0f, 0.0f),
    glm::vec3(-1.0f, 0.0f, 0.0f),
    glm::vec3(0.0f, 1.0f, 0.0f),
    glm::vec3(0.0f, -1.0f, 0.0f),
    glm::vec3(0.0f, 0.0f, 1.0f),
    glm::vec3(0.0f, 0.0f, -1.0f),
  };

  auto indices = std::vector<u32>();
  for (const auto& normal : FACES) {
    const auto u = glm::vec3(normal.y, normal.z, normal.x);
    const auto v = glm::cross(normal, u);
    const auto first = static_cast<u32>(positions.size());
    for (const auto& corner : {-u - v, u - v, u + v, -u + v}) {
      positions.push_back((normal + corner) * half_extent);
      normals.push_back(normal);
    }

    for (const auto offset : {0_u32, 1_u32, 2_u32, 0_u32, 2_u32, 3_u32}) {
      indices.push_back(first + offset);
    }
  }

  return indices;
}

TEST(MeshBuilderTest, BuildsOneLodWhenAsked) {
  auto source = MeshSource{};
  source.indices = box_triangles(glm::vec3(1.0f, 2.0f, 3.0f), source.positions, source.normals);

  const auto mesh = build_mesh(source, {}, 1);

  ASSERT_TRUE(mesh.has_value());
  EXPECT_EQ(mesh->lod_count, 1);
  EXPECT_EQ(mesh->vertex_count, 24);
  EXPECT_EQ(mesh->lods[0].indices_count, 36);
  EXPECT_GT(mesh->lods[0].meshlet_count, 0);
  EXPECT_EQ(mesh->lod_metadata_offset + sizeof(GPU::MeshLOD), mesh->blob.size());
  EXPECT_FLOAT_EQ(mesh->bounds_extent[0], 2.0f);
  EXPECT_FLOAT_EQ(mesh->bounds_extent[1], 4.0f);
  EXPECT_FLOAT_EQ(mesh->bounds_extent[2], 6.0f);
}

TEST(MeshBuilderTest, KeepsTheTrianglesUnquantizedForPhysics) {
  auto source = MeshSource{};
  source.indices = box_triangles(glm::vec3(0.0123f, 0.0456f, 0.0789f), source.positions, source.normals);

  const auto mesh = build_mesh(source, {}, 1);

  ASSERT_TRUE(mesh.has_value());
  ASSERT_EQ(mesh->collision_positions.size(), source.positions.size() * 3);
  ASSERT_EQ(mesh->collision_indices.size(), source.indices.size());
  for (auto i = 0_sz; i < mesh->collision_indices.size(); i++) {
    const auto index = mesh->collision_indices[i];
    const auto position = glm::vec3(
      mesh->collision_positions[index * 3],
      mesh->collision_positions[index * 3 + 1],
      mesh->collision_positions[index * 3 + 2]
    );
    EXPECT_EQ(position, source.positions[source.indices[i]]);
  }
}

TEST(MeshBuilderTest, RejectsMalformedSources) {
  auto source = MeshSource{};
  source.indices = box_triangles(glm::vec3(1.0f), source.positions, source.normals);

  auto short_indices = source;
  short_indices.indices.pop_back();
  auto out_of_range = source;
  out_of_range.indices[5] = static_cast<u32>(source.positions.size());

  EXPECT_FALSE(build_mesh(short_indices, {}, 1).has_value());
  EXPECT_FALSE(build_mesh(out_of_range, {}, 1).has_value());
  EXPECT_FALSE(build_mesh(MeshSource{}, {}, 1).has_value());
}

TEST(MeshBuilderTest, BuildsARuntimeModelWithAMaterialPerMesh) {
  auto skin_positions = std::vector<glm::vec3>();
  auto skin_normals = std::vector<glm::vec3>();
  const auto skin_indices = box_triangles(glm::vec3(1.0f), skin_positions, skin_normals);
  auto cut_positions = std::vector<glm::vec3>();
  auto cut_normals = std::vector<glm::vec3>();
  auto cut_indices = box_triangles(glm::vec3(0.5f), cut_positions, cut_normals);
  cut_indices.resize(6);

  const auto skin_material = UUID::generate_random();
  const auto meshes = std::array{
    RuntimeMeshSource{
      .positions = skin_positions,
      .normals = skin_normals,
      .indices = skin_indices,
      .material = skin_material,
    },
    RuntimeMeshSource{.positions = cut_positions, .normals = cut_normals, .indices = cut_indices},
  };

  const auto model = build_runtime_model(meshes);

  ASSERT_TRUE(model.has_value());
  ASSERT_EQ(model->meshes.size(), 2);
  ASSERT_EQ(model->mesh_groups.size(), 1);
  EXPECT_EQ(model->mesh_groups[0].mesh_indices, (std::vector<u32>{0, 1}));
  EXPECT_TRUE(model->mesh_groups[0].child_indices.empty());
  ASSERT_EQ(model->materials.size(), 1);
  EXPECT_EQ(model->materials[0].uuid.unpack(), skin_material);
  EXPECT_EQ(model->meshes[0].material_index, 0);
  EXPECT_EQ(model->meshes[1].material_index, ModelData::INVALID_INDEX);
  EXPECT_EQ(model->meshes[0].lod_count, 1);
  EXPECT_EQ(model->meshes[1].lod_count, 1);
  EXPECT_EQ(model->meshes[1].collision_indices.size(), 6);
}

TEST(MeshBuilderTest, LeavesOutMeshesWithoutTriangles) {
  auto positions = std::vector<glm::vec3>();
  auto normals = std::vector<glm::vec3>();
  const auto indices = box_triangles(glm::vec3(1.0f), positions, normals);
  const auto meshes = std::array{
    RuntimeMeshSource{},
    RuntimeMeshSource{.positions = positions, .normals = normals, .indices = indices},
  };

  const auto model = build_runtime_model(meshes);

  ASSERT_TRUE(model.has_value());
  ASSERT_EQ(model->meshes.size(), 1);
  EXPECT_EQ(model->mesh_groups[0].mesh_indices, (std::vector<u32>{0}));
  EXPECT_FALSE(build_runtime_model(std::array{RuntimeMeshSource{}}).has_value());
}

TEST(MeshBuilderTest, RefusesARuntimeMeshWithTheWrongNormalCount) {
  auto positions = std::vector<glm::vec3>();
  auto normals = std::vector<glm::vec3>();
  const auto indices = box_triangles(glm::vec3(1.0f), positions, normals);
  normals.pop_back();

  const auto meshes = std::array{
    RuntimeMeshSource{.positions = positions, .normals = normals, .indices = indices},
  };

  EXPECT_FALSE(build_runtime_model(meshes).has_value());
}
