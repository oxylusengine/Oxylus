#include <SDL3/SDL_error.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <fmt/format.h>

#include "Memory/Stack.hpp"
#include "OS/OS.hpp"

namespace ox {
auto os::spawn_detached(std::span<const std::string_view> args) -> std::expected<void, std::string> {
  ZoneScoped;
  memory::ScopedStack stack;

  if (args.empty()) {
    return std::unexpected("Couldn't run an empty command");
  }

  auto argv = stack.alloc<const c8*>(args.size() + 1);
  for (usize i = 0; i < args.size(); i++) {
    argv[i] = stack.null_terminate_cstr(args[i]);
  }
  argv[args.size()] = nullptr;

  const auto properties = SDL_CreateProperties();
  SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, static_cast<void*>(argv.data()));
  SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);

  auto* process = SDL_CreateProcessWithProperties(properties);
  SDL_DestroyProperties(properties);
  if (!process) {
    return std::unexpected(fmt::format("Couldn't run {}: {}", args[0], SDL_GetError()));
  }

  SDL_DestroyProcess(process);

  return {};
}
} // namespace ox
