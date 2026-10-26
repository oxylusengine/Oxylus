#include "Core/Version.hpp"

#include "OxylusBuildInfo.hpp"

namespace ox {
static constexpr EngineBuildInfo BUILD_INFO{
  .major = OX_VERSION_MAJOR,
  .minor = OX_VERSION_MINOR,
  .patch = OX_VERSION_PATCH,
  .version = OX_VERSION,
  .full_version = OX_VERSION_FULL,
  .revision = OX_VERSION_REVISION,
  .build_mode = OX_VERSION_BUILD_MODE,
  .dirty = OX_VERSION_DIRTY,
};

auto engine_build_info() -> const EngineBuildInfo& { return BUILD_INFO; }
} // namespace ox
