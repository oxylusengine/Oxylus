#pragma once

#include <flecs.h>

// clang-format off
#define ECS_EXPORT_TYPES
#include "Scene/ECSModule/Reflect.hpp"

#include "Scene/Components.hpp"
#undef ECS_EXPORT_TYPES

#undef ECS_COMPONENT_BEGIN
#undef ECS_COMPONENT_END
#undef ECS_COMPONENT_MEMBER
#undef ECS_COMPONENT_TAG
// clang-format on
namespace ox {
struct Core {
  Core(flecs::world& world);
};
} // namespace ox
