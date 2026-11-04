#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_loadso.h>
#include <array>
#include <coreclr_delegates.h>
#include <fmt/format.h>
#include <hostfxr.h>
#include <nethost.h>

#include "Core/Base.hpp"
#include "Core/Option.hpp"
#include "DotNetHosts.hpp"
#include "Utils/Log.hpp"

namespace ox {
#if defined(OX_PLATFORM_WINDOWS)
constexpr auto BOOTSTRAP_TYPE = L"Oxylus.Hosting.Bootstrap, Oxylus.Scripting";
constexpr auto BOOTSTRAP_METHOD = L"Load";
#else
constexpr auto BOOTSTRAP_TYPE = "Oxylus.Hosting.Bootstrap, Oxylus.Scripting";
constexpr auto BOOTSTRAP_METHOD = "Load";
#endif

#if defined(OX_PLATFORM_WINDOWS)
constexpr auto NETHOST_LIBRARY = "nethost.dll";
#elif defined(OX_PLATFORM_MACOSX)
constexpr auto NETHOST_LIBRARY = "libnethost.dylib";
#else
constexpr auto NETHOST_LIBRARY = "libnethost.so";
#endif

using BootstrapLoadFn = auto (*)(
  const c8* game_assembly,
  i32 game_assembly_length,
  const OxNativeApi* native_api,
  OxManagedApi* managed_api,
  u32 abi_version
) -> OxStatus;

using GetHostFxrPathFn =
  auto(NETHOST_CALLTYPE*)(char_t* buffer, size_t* buffer_size, const get_hostfxr_parameters* parameters) -> i32;

// CoreCLR can only be started once per process, so this outlives every load and unload
struct HostFxrRuntime {
  std::filesystem::path scripting_directory = {};
  BootstrapLoadFn bootstrap_load = nullptr;
};

static auto to_char_t(const std::filesystem::path& path) -> std::basic_string<char_t> {
#if defined(OX_PLATFORM_WINDOWS)
  return path.wstring();
#else
  return path.string();
#endif
}

static auto from_char_t(const char_t* str) -> std::filesystem::path { return std::filesystem::path(str); }

static auto load_symbol(SDL_SharedObject* library, const c8* name) -> void* {
  return reinterpret_cast<void*>(SDL_LoadFunction(library, name));
}

// next to the executable first, which is where the build copies it, then wherever the OS looks
static auto load_nethost() -> SDL_SharedObject* {
  if (const auto* base_path = SDL_GetBasePath()) {
    const auto local = std::filesystem::path(base_path) / NETHOST_LIBRARY;
    if (auto* library = SDL_LoadObject(to_utf8(local).c_str())) {
      return library;
    }
  }

  return SDL_LoadObject(NETHOST_LIBRARY);
}

static auto start_runtime(const std::filesystem::path& scripting_directory)
  -> std::expected<HostFxrRuntime, std::string> {
  ZoneScoped;

  auto* nethost = load_nethost();
  if (!nethost) {
    return std::unexpected(fmt::format("Couldn't load {}: {}", NETHOST_LIBRARY, SDL_GetError()));
  }

  const auto get_hostfxr_path = reinterpret_cast<GetHostFxrPathFn>(load_symbol(nethost, "get_hostfxr_path"));
  if (!get_hostfxr_path) {
    return std::unexpected(fmt::format("{} doesn't export get_hostfxr_path", NETHOST_LIBRARY));
  }

  auto hostfxr_path_buffer = std::array<char_t, 1024>{};
  auto hostfxr_path_size = hostfxr_path_buffer.size();
  if (get_hostfxr_path(hostfxr_path_buffer.data(), &hostfxr_path_size, nullptr) != 0) {
    return std::unexpected(std::string("Couldn't find hostfxr, is the .NET 10 runtime installed?"));
  }

  const auto hostfxr_path = from_char_t(hostfxr_path_buffer.data());
  auto* hostfxr = SDL_LoadObject(to_utf8(hostfxr_path).c_str());
  if (!hostfxr) {
    return std::unexpected(fmt::format("Couldn't load {}: {}", to_utf8(hostfxr_path), SDL_GetError()));
  }

  const auto initialize = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
    load_symbol(hostfxr, "hostfxr_initialize_for_runtime_config")
  );
  const auto get_delegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
    load_symbol(hostfxr, "hostfxr_get_runtime_delegate")
  );
  const auto close = reinterpret_cast<hostfxr_close_fn>(load_symbol(hostfxr, "hostfxr_close"));
  if (!initialize || !get_delegate || !close) {
    return std::unexpected(fmt::format("{} is missing the hosting exports", to_utf8(hostfxr_path)));
  }

  const auto runtime_config = scripting_directory / "Oxylus.Scripting.runtimeconfig.json";
  auto context = hostfxr_handle{nullptr};
  const auto init_result = initialize(to_char_t(runtime_config).c_str(), nullptr, &context);
  // 1 and 2 mean a runtime was already running, which still hands back a usable context
  if (init_result < 0 || !context) {
    return std::unexpected(
      fmt::format(
        "Couldn't start the .NET runtime from {}: {:#x}",
        to_utf8(runtime_config),
        static_cast<u32>(init_result)
      )
    );
  }

  OX_DEFER(&) { close(context); };

  auto load_assembly = load_assembly_fn{nullptr};
  auto get_function_pointer = get_function_pointer_fn{nullptr};
  if (
    get_delegate(context, hdt_load_assembly, reinterpret_cast<void**>(&load_assembly)) != 0 ||
    get_delegate(context, hdt_get_function_pointer, reinterpret_cast<void**>(&get_function_pointer)) != 0
  ) {
    return std::unexpected(std::string("Couldn't get the .NET hosting delegates"));
  }

  // the default context, so a game's collectible context resolves Oxylus.Scripting to this one copy
  const auto scripting_assembly = scripting_directory / "Oxylus.Scripting.dll";
  if (const auto result = load_assembly(to_char_t(scripting_assembly).c_str(), nullptr, nullptr); result != 0) {
    return std::unexpected(
      fmt::format("Couldn't load {}: {:#x}", to_utf8(scripting_assembly), static_cast<u32>(result))
    );
  }

  auto runtime = HostFxrRuntime{.scripting_directory = scripting_directory};
  const auto bootstrap_result = get_function_pointer(
    BOOTSTRAP_TYPE,
    BOOTSTRAP_METHOD,
    UNMANAGEDCALLERSONLY_METHOD,
    nullptr,
    nullptr,
    reinterpret_cast<void**>(&runtime.bootstrap_load)
  );
  if (bootstrap_result != 0 || !runtime.bootstrap_load) {
    return std::unexpected(
      fmt::format("Couldn't find Oxylus.Hosting.Bootstrap.Load: {:#x}", static_cast<u32>(bootstrap_result))
    );
  }

  return runtime;
}

auto hostfxr_load(
  const std::filesystem::path& game_assembly,
  const std::filesystem::path& scripting_directory_override,
  const OxNativeApi& native_api,
  OxManagedApi& managed_api,
  u32 abi_version
) -> std::expected<void, std::string> {
  ZoneScoped;

  static auto runtime = option<HostFxrRuntime>(nullopt);

  const auto scripting_directory = scripting_directory_override.empty() ? game_assembly.parent_path()
                                                                        : scripting_directory_override;
  if (!runtime.has_value()) {
    auto started = start_runtime(scripting_directory);
    if (!started) {
      return std::unexpected(std::move(started.error()));
    }

    runtime = std::move(*started);
  } else if (runtime->scripting_directory != scripting_directory) {
    OX_LOG_WARN(
      "Oxylus.Scripting stays loaded from {}, {} can't bring its own copy",
      to_utf8(runtime->scripting_directory),
      to_utf8(game_assembly)
    );
  }

  const auto path = to_utf8(game_assembly);
  const auto status = runtime->bootstrap_load(
    path.data(),
    static_cast<i32>(path.size()),
    &native_api,
    &managed_api,
    abi_version
  );
  if (status != OxStatus::Ok) {
    return std::unexpected(fmt::format("Couldn't load {}: {}", path, status_message(status)));
  }

  return {};
}
} // namespace ox
