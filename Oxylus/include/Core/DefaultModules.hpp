#pragma once

#include <tuple>

#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/Input.hpp"
#include "Networking/NetworkManager.hpp"
#include "Physics/Physics.hpp"
#include "Render/DebugRenderer.hpp"
#include "Render/Renderer.hpp"
#include "Render/RendererConfig.hpp"
#include "Scripting/LuaManager.hpp"
#include "UI/ImGuiRenderer.hpp"

namespace ox {
using DefaultModules = std::tuple<
  LuaManager,
  AssetManager,
  AudioEngine,
  Physics,
  Input,
  NetworkManager,
  RendererConfig,
  Renderer,
  DebugRenderer,
  ImGuiRenderer>;
}
