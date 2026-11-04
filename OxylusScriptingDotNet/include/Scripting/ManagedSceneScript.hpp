#pragma once

#include <string>

#include "Scripting/SceneScript.hpp"

struct ecs_world_t;

namespace ox {
struct DotNetScripting;

// a C# scene behaviour in one scene. The instance lives on the managed side behind `handle`, and the scene's runtime
// dispatches its hooks together with every other managed script in the world, so this only creates and destroys it
class ManagedSceneScript final : public SceneScript {
public:
  const std::string type_name;
  // 0 while the scripts aren't loaded, the module recreates the instance when they are
  u64 handle = 0;

  ManagedSceneScript(DotNetScripting& module, ecs_world_t* world, std::string type_name_);

  auto display_name() const -> std::string override;

  auto on_add(Scene* scene) -> void override;
  auto on_remove(Scene* scene) -> void override;

private:
  DotNetScripting& module_;
  ecs_world_t* world_ = nullptr;
};
} // namespace ox
