#pragma once

#include <tuple>

#include "Asset/AssetManager.hpp"
#include "Audio/AudioEngine.hpp"
#include "Core/Input.hpp"
#include "Networking/NetworkManager.hpp"
#include "Physics/Physics.hpp"
#include "Render/Renderer.hpp"
#include "Scripting/LuaManager.hpp"
#include "UI/ImGuiRenderer.hpp"
#include "UI/RmlUI.hpp"

// defined by linking OxylusDotNet, which owns the header
#ifdef OX_DOTNET
  #include "Scripting/DotNetScripting.hpp"
#endif

namespace ox {
#ifdef OX_DOTNET
using DefaultModules = std::tuple<
  LuaManager,
  AudioEngine,
  AssetManager,
  Physics,
  Input,
  DotNetScripting,
  NetworkManager,
  Renderer,
  ImGuiRenderer,
  RmlUI>;
#else
using DefaultModules =
  std::tuple<LuaManager, AudioEngine, AssetManager, Physics, Input, NetworkManager, Renderer, ImGuiRenderer, RmlUI>;
#endif
} // namespace ox
