#include <flecs.h>
#include <gtest/gtest.h>

#include "Core/App.hpp"
#include "Scene/ComponentReflection.hpp"
#include "Scene/Components.hpp"
#include "Scene/Scene.hpp"

#ifdef OX_LUA_BINDINGS
  #include "Scripting/LuaManager.hpp"
#endif

namespace ox {
// the editor's Add Component popup lists every child of the Core module
TEST(CoreComponents, AssetFieldsIsNotAGameplayComponent) {
  c8 arg0[] = "TestCoreComponents";
  c8* argv[] = {arg0, nullptr};
  auto app = App(1, argv);

#ifdef OX_LUA_BINDINGS
  app.with<LuaManager>();
  ASSERT_TRUE(App::mod<LuaManager>().init().has_value());
#endif

  auto world = flecs::world{};
  auto component_db = ComponentDB{};
  component_db.import_module(world.import<CoreComponentsModule>());

  EXPECT_TRUE(component_db.is_component_known(world.component<MeshComponent>()));
  EXPECT_FALSE(component_db.is_component_known(world.component<AssetFields>()));
}
} // namespace ox
