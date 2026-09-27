#include <array>
#include <flecs.h>
#include <gtest/gtest.h>
#include <string_view>

#include "Asset/AssetFile.hpp"
#include "Core/UUID.hpp"
#include "Scene/ComponentRegistry.hpp"

namespace ox {
struct AssetHolder {
  UUID model = {};
  u32 index = 0;
  UUID texture = {};
};

static constexpr auto HOLDER_ASSET_FIELDS = std::array{
  AssetField{"model", AssetType::Model},
  AssetField{"texture", AssetType::Texture},
};

static auto bind_holder(flecs::world& world) -> flecs::entity {
  auto component = world.component<AssetHolder>("AssetHolder");
  component.member("model", &AssetHolder::model);
  component.member("index", &AssetHolder::index);
  component.member("texture", &AssetHolder::texture);
  return ComponentBuilder{component}.asset_fields(HOLDER_ASSET_FIELDS);
}

// every Scene imports the component module into its own world, the editor makes one per thumbnail
TEST(ComponentRegistry, AssetFieldsSurviveSeveralWorlds) {
  for (auto i = 0; i < 3; i++) {
    auto world = flecs::world{};
    world.component<UUID>("ox::UUID");
    world.component<AssetFields>("AssetFields");

    const auto component = bind_holder(world);
    const auto* asset_fields = component.try_get<AssetFields>();
    ASSERT_NE(asset_fields, nullptr);
    ASSERT_EQ(asset_fields->fields.size(), 2u);
    EXPECT_EQ(std::string_view(asset_fields->fields[0].member), "model");
    EXPECT_EQ(asset_fields->fields[0].type, AssetType::Model);
    EXPECT_EQ(std::string_view(asset_fields->fields[1].member), "texture");
    EXPECT_EQ(asset_fields->fields[1].type, AssetType::Texture);
  }
}
} // namespace ox
