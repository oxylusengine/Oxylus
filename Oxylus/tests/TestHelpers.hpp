#pragma once

#include "Scene/Scene.hpp"

inline auto create_test_scene() -> std::unique_ptr<ox::Scene> { return std::make_unique<ox::Scene>("TestScene"); }

inline auto log_test_end() -> void { OX_LOG_INFO("============================"); }
