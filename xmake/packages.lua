add_requires("stb 2024.06.01")

add_requires("miniaudio 0.11.22")

add_requires("imgui d896eab16620f80e3cae9b164eb62f71b47f6e45", { configs = {
    wchar32 = true,
    debug = is_mode("debug")
} })

add_requires("imguizmo-lr v1.91.8-docking")

add_requires("glm 1.0.1", { configs = {
    header_only = true,
    cxx_standard = "20",
}, system = false })

add_requires("flecs v4.0.5")

add_requires("fastgltf v0.8.0", { system = false, debug = is_mode("debug") })

add_requires("meshoptimizer v0.22")

add_requires("fmt 11.1.4", { configs = {
    header_only = true
}, system = false })

add_requires("loguru v2.1.0", { configs = {
    fmt = true,
}, system = false })

add_requires("vk-bootstrap v1.4.307", { system = false, debug = is_mode("debug") })

add_requires("vuk 2025.06.15", { configs = {
    debug_allocations = false,
}, debug = is_mode("debug") })

add_requires("shader-slang v2025.10.4", { system = false })

add_requires("libsdl3", { configs = {
    wayland = false,
    x11 = true,
} })

add_requires("toml++ v3.4.0")

add_requires("simdjson v3.12.2")

add_requires("joltphysics-ox v5.3.0", { configs = {
    debug_renderer = true,
    rtti = true,
    avx = true,
    avx2 = true,
    lzcnt = true,
    sse4_1 = true,
    sse4_2 = true,
    tzcnt = true,
    enable_floating_point_exceptions = false,
} })

add_requires("tracy v0.11.1", { configs = {
    tracy_enable = has_config("profile"),
    on_demand = true,
    callstack = true,
    callstack_inlines = false,
    code_transfer = true,
    exit = true,
    system_tracing = true,
} })

add_requires("sol2 c1f95a773c6f8f4fde8ca3efe872e7286afe4444")

add_requires("enkits v1.11")

add_requires("unordered_dense v4.5.0")

add_requires("plf_colony v7.41")

add_requires("dylib v2.2.1")

add_requires("zstd v1.5.7", { system = false })
add_requires("ktx v4.4.0", { system = false })

add_requires("simdutf v6.2.0")
