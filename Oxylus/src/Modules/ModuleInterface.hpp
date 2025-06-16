#pragma once

#define OX_BUILD_DLL
#include <imgui_internal.h>
#include <sol/state.hpp>

#include "Core/App.hpp"
#include "Linker.hpp"

namespace ox {
class OX_SHARED ModuleInterface {
public:
  virtual ~ModuleInterface() = default;

  virtual void init(App* app_instance, ImGuiContext* imgui_context) = 0;
};

// use this function return a heap allocated ModuleInterface
#define CREATE_MODULE_FUNC extern "C" OX_SHARED ModuleInterface* create_module()
} // namespace ox
