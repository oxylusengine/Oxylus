#include <SDL3/SDL_error.h>
#include <SDL3/SDL_loadso.h>
#include <fmt/format.h>

#include "DotNetHosts.hpp"

namespace ox {
auto aot_load(
  const std::filesystem::path& library, const OxNativeApi& native_api, OxManagedApi& managed_api, u32 abi_version
) -> std::expected<void, std::string> {
  ZoneScoped;

  // never unloaded: a NativeAOT runtime can't be torn down, so a reload re-runs init on the same image
  auto* handle = SDL_LoadObject(to_utf8(library).c_str());
  if (!handle) {
    return std::unexpected(fmt::format("Couldn't load {}: {}", to_utf8(library), SDL_GetError()));
  }

  const auto init = reinterpret_cast<OxManagedInitFn>(SDL_LoadFunction(handle, "ox_managed_init"));
  if (!init) {
    return std::unexpected(fmt::format("{} doesn't export ox_managed_init", to_utf8(library)));
  }

  const auto status = init(&native_api, &managed_api, abi_version);
  if (status != OxStatus::Ok) {
    return std::unexpected(fmt::format("Couldn't initialize {}: {}", to_utf8(library), status_message(status)));
  }

  return {};
}
} // namespace ox
