#pragma once

#include "Scene/ECSModule/Core.hpp"
namespace ox {
struct SkyboxLoadEvent {
  Shared<Texture> cube_map = nullptr;
};
} // namespace ox
