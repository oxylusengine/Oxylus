# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build system

Xmake (not CMake). Requires a C++23 compiler and the Vulkan SDK. Packages come from a custom
repo declared in the root `xmake.lua` (`oxylus https://github.com/oxylusengine/xmake-repo.git`);
`package.precompiled` is disabled, so first configure builds dependencies from source and is slow.

```bash
# Configure (pick toolchain from xmake/toolchains.lua: clang, clang-cl, nix-clang, mac-clang, ...)
xmake f --toolchain=nix-clang --runtimes=c++_static -m debug

# Build — ALWAYS cap parallelism at 8; unbounded -j has crashed this machine
xmake b -j 8
xmake b -j 8 Oxylus          # single target
xmake b -j 8 -a              # all targets, including non-default ones (tests)

xmake r OxylusEditor         # run the editor
```

On NixOS, use `nix-shell` (see `shell.nix`) and `--toolchain=nix-clang`; the shell pins libc++,
Vulkan loader, and `shader-slang`.

Configure options (`xmake f --<opt>=<val>`): `lua_bindings` (default true), `editor` (default true),
`tests` (default false), `profile` (Tracy, default false), `llvmpipe` (force software Vulkan device).

Build modes are `debug`, `release`, `dist`. Each defines `OX_DEBUG` / `OX_RELEASE` / `OX_DISTRIBUTION`.
Output lands in `build/<plat>/<arch>/<mode>/`, with resources and compiled shader packs copied next to
the binary. `compile_commands.json` is auto-regenerated into `build/` for clangd.

## Tests

Tests are GoogleTest binaries under `Oxylus/tests/**/Test*.cpp`. Each file becomes its own target
(named after the file) via the loop in `Oxylus/tests/xmake.lua`, and all are `set_default(false)`.
ASan/UBSan (plus LSan on Linux) are forced on for test targets, so tests are slow.

```bash
xmake f --tests=y           # must be enabled at configure time
xmake b -j 8 -a             # tests are non-default targets
xmake test                  # all tests
xmake test TestScene/*      # one target's tests (test name is "default")
./build/linux/x86_64/debug/TestScene --gtest_filter=Foo.Bar   # run the binary directly
```

Adding a test = dropping a new `Test*.cpp` under `Oxylus/tests/`; no xmake edit needed.

## Targets

- **Oxylus** (`Oxylus/`) — static engine library. Public headers in `include/`, implementation in
  `src/` mirroring the same directory names. Platform files are selected by filename: `src/OS/Win32*`,
  `src/OS/Linux*`, `src/OS/MacOS*` are removed for non-matching platforms.
- **ResourceCompiler** (`ResourceCompiler/`) — shared library wrapping slang; compiles `.slang`
  shaders into `.oxpack` archives.
- **rcli** — CLI front end for ResourceCompiler, invoked at build time by the `ox.compile_shaders`
  rule. It sets `build.fence` so dependents never compile before it exists.
- **OxylusEditor** (`OxylusEditor/`) — ImGui editor executable; `App` + `DefaultModules` + `Editor`.

## Architecture

### App and modules

`ox::App` (`Core/App.hpp`) is a singleton assembled with a fluent builder in `main()`, then `.run()`:

```cpp
ox::App(argc, argv).with_name(name).with_window(...).with(ox::DefaultModules{}).with<ox::Editor>().run();
```

A **module** is any type satisfying the `Module` concept in `Core/ModuleRegistry.hpp`: it has
`init()`, `deinit()`, and a `static constexpr MODULE_NAME`. `update(const Timestep&)` and
`render(vuk::Extent3D, vuk::Format)` are optional and detected via concepts. Declare a
`using module_dependencies = std::tuple<...>` member and the registry fatal-errors at `add()` time if
a dependency is missing — so **registration order matters**. `init`/`deinit` return
`std::expected<void, std::string>`.

Access modules statically: `App::mod<Renderer>()`, `App::has_mod<Physics>()`. Core services are not
modules and have their own accessors: `App::get_vfs()`, `get_job_manager()`, `get_event_system()`,
`get_rendercontext()`, `get_window()`, `get_timestep()`. `App::defer_to_next_frame(fn)` queues work.

`Core/DefaultModules.hpp` is the canonical registration order: LuaManager, AssetManager, AudioEngine,
Physics, Input, NetworkManager, Renderer, DebugRenderer, ImGuiRenderer, RmlUI.

`EventSystem` (`Core/EventSystem.hpp`) is a typed pub/sub bus keyed on `std::type_index`; event types
are plain copyable structs (`WindowResizeEvent`, `AppCloseEvent`, `Editor::ScenePlayEvent`, ...).

`VFS` (`Core/VFS.hpp`) maps virtual dirs to physical ones. `App::init` mounts `VFS::APP_DIR` to the
assets path (`Resources` by default, override with `with_assets_directory`); `VFS::PROJECT_DIR` is
editor-only. Runtime asset paths go through `resolve_physical_dir`.

### Scene / ECS

`ox::Scene` (`Scene/Scene.hpp`) owns a `flecs::world` and is the unit of gameplay. It is much more
than an ECS wrapper — it also owns the GPU-side mirrors of scene state:

- `SlotMap<GPU::Transforms, GPU::TransformID> transforms` plus `entity_transforms_map` and
  `dirty_transforms`; `set_dirty(entity)` marks a transform for re-upload.
- `mesh_instances`, `lights`, `gpu_materials`, and `dirty_mesh_instances` similarly feed the renderer.
- A `RendererInstance` (one per scene) and a Jolt `PhysicsSystem` with contact/activation listeners.
- `ComponentDB`, which tracks flecs component ids imported from flecs modules
  (`CoreComponentsModule` in `Scene/Components.hpp`) so serialization and the inspector know what
  components exist.

Systems and observers are all registered imperatively in `Scene::init` in `Oxylus/src/Scene/Scene.cpp`
(a large function): observers keep the GPU mirrors and physics bodies in sync with component
add/remove, and named systems (`physics_step`, `rigidbody_update`, `camera_update`,
`sprite_animation_update`, ...) run per frame. `runtime_start`/`runtime_stop`/`runtime_update` drive
play mode; `disable_phases`/`enable_all_phases` gate flecs phases (the editor uses this to freeze
gameplay while still rendering). Physics runs on a fixed `physics_interval` accumulator.

Scenes serialize to JSON (`to_json`/`from_json`, `entity_to_json`/`json_to_entity`) using `simdjson`
for reading and `JsonWriter` for writing.

### Assets

`AssetManager` is the single owner of loaded resources. Every asset is a `UUID` in an `AssetRegistry`
map; the `Asset` struct holds a type tag plus a union of typed slot-map ids (`ModelID`, `TextureID`,
`MaterialID`, `SceneID`, `AudioID`, `ScriptID`). Payloads live in per-type `SlotMap`s guarded by
per-type `std::shared_mutex`, and accessors return `ReadGuard<T>` (`Memory/ReadGuard.hpp`) which
holds the lock — don't store a `ReadGuard` past its use site. Loading is reference-counted
(`acquire_ref`/`release_ref`, atomic `ref_count`); `load_asset` acquires by default.

### Rendering

vuk-based, with a bindless descriptor set held by `RenderContext`. `Renderer` is the module (owns
shared resources and pipelines); `RendererInstance` is per-scene and builds the frame.

Shaders are Slang (`Oxylus/src/Render/Shaders/`, editor-only ones under `Shaders/editor/`). They are
**not** compiled by name discovery: every shader program must be declared in a TOML manifest —
`OxylusEditor/Resources/engine.toml` and `editor.toml` — listing `name`, `path`, `entry_points`, and
`bindless`. The `ox.compile_shaders` xmake rule (`xmake/rules.lua`) feeds each TOML to `rcli`, which
produces `engine.oxpack` / `editor.oxpack` next to the binary. At runtime `Renderer::init` unpacks
`engine.oxpack` and calls `RenderContext::create_pipeline` for each entry. **Adding a shader means
editing the TOML**, and the rule parses the TOML to register `.slang` files as build dependencies.

`RendererInstance.hpp` defines the frame structure: a fixed `RenderStage` enum (Initialization,
Culling, VisBufferEncode/Decode, Forward2D, Lighting, PostProcessing, Atmosphere, Debug, FinalOutput)
into which callbacks are injected via `StageDependency{target_stage, Before/After, order}`.
`RenderStageContext` passes named `vuk::Value<Buffer>` / `vuk::Value<ImageAttachment>` resources
between stages, with a `SharedResources` tier that persists across stages. Pass implementations live
in `Oxylus/src/Render/Passes/`.

Rendering is configured through the CVar system (`Utils/CVars.hpp`): `RendererCVar` is **per-scene**
(serialized with the scene), while `ContextCVar` is global and persisted to `context_config.toml`.

### Scripting

Lua via sol2, compiled only when `lua_bindings` is on (`OX_LUA_BINDINGS`; the option strips
`src/Scripting/*Bindings*` when disabled). `LuaManager` is the module holding the `sol::state` and a
name-keyed map of `LuaBinding` subclasses (`bind(sol::state*)`), one per subsystem
(`LuaFlecsBindings`, `LuaSceneBindings`, `LuaPhysicsBindings`, ...).

`LuaSystem` is a loaded script asset. `Scene` keeps `lua_systems` keyed by script UUID and forwards
lifecycle hooks: `on_add`/`on_remove`, `on_scene_start`/`stop`/`update`/`fixed_update`/`render`, and
Jolt contact callbacks. Scripts can define flecs systems, so gameplay can be written entirely in Lua.

## Conventions

- `namespace ox` for everything in the engine and editor.
- Short numeric typedefs from `Core/Types.hpp` are global: `u32`, `i64`, `f32`, `usize`, `c8`, ...
  Prefer these over `uint32_t`/`float`.
- Trailing return types with **explicit object parameters** are the house style:
  `auto foo(this Scene& self, ...) -> void`. Use `self.` rather than implicit member access in those.
- `ox::option<T>` / `ox::nullopt` (`Core/Option.hpp`) instead of `std::optional`. For enums with an
  `Invalid` member and for unsigned/float types it collapses to a flag-value representation with no
  extra storage — hence the `enum class XxxID : u64 { Invalid = max() }` pattern everywhere.
- Recoverable failure: `std::expected<void, std::string>`. Programmer error: the `OX_*` macros.
- Logging/assertion macros from `Utils/Log.hpp`: `OX_LOG_INFO/WARN/ERROR/FATAL/DEBUG/TRACE`
  (fmt-style), `OX_ASSERT`, `OX_CHECK_NULL/EQ/NE/LT/GT/LE/GE`. Backed by loguru.
- Profiling: `tracy/Tracy.hpp` is force-included into every Oxylus TU. Put `ZoneScoped;` at the top of
  non-trivial functions, `ZoneScopedN("name")` inside lambdas. No-ops unless `--profile=y`.
- Containers: `ankerl::unordered_dense::map` over `std::unordered_map`, `SlotMap` for id-addressed
  storage, `plf::colony` and `svector` are available.
- `OX_DEFER(...)` for scope cleanup (`Core/Base.hpp`).
- Formatting is enforced by `.clang-format` (2-space indent, 120 columns, `BlockIndent` brackets,
  one-arg-per-line binpacking). Run `clang-format -i` on files you touch.
- Warnings are aggressive (`allextra`, `pedantic`, `-Wshadow`/`-Wshadow-all`) though not fatal;
  shadowing in particular is treated as a bug here, so don't reuse names from an enclosing scope.

## CI

`.github/workflows/xmake.yaml` builds Windows/msvc, Linux/clang-20, and macOS/mac-clang in both debug
and release with `--tests=false`, then `xmake build -a` and `xmake install`. It does **not** run
tests, so verify tests locally.
