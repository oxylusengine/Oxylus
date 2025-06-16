#pragma once

// clang-format off

#include <cstdlib>
#include <cstdint>
#include <cstddef>

#include <source_location>
#include <string>
#include <string_view>
#include <filesystem>
#include <limits>
#include <utility>
#include <exception>
#include <optional>
#include <memory>
#include <type_traits>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <ranges>
#include <map>
#include <thread>
#include <queue>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <span>
#include <expected>
#include <chrono>

namespace fs = std::filesystem;

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

#include <glm/gtx/compatibility.hpp>
#include <glm/gtx/component_wise.hpp>
#include <glm/gtx/quaternion.hpp>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <ankerl/unordered_dense.h>

#include <plf_colony.h>

#include <Jolt/Core/Core.h>

#include "Core/Types.hpp"
#include "Core/Base.hpp"
#include "Core/Enum.hpp"
#include "Core/Option.hpp"

#include "Utils/Log.hpp"
#include "Utils/Profiler.hpp"

#define OX_CALLSTACK std::source_location LOC
#define OX_THISCALL OX_CALLSTACK = std::source_location::current()

// clang-format on
