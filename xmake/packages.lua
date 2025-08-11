add_requires("stb 2024.06.01")

add_requires("miniaudio 0.11.22")

local imgui_version = "v1.92.0-docking"
local imgui_configs = { wchar32 = true }
add_requires("imgui " .. imgui_version, { configs = imgui_configs })

add_requires("implot 3da8bd34299965d3b0ab124df743fe3e076fa222")
add_requireconfs("imgui", "implot.imgui", {
    override = true, version = imgui_version, configs = imgui_configs
})

add_requires("imguizmo 1.91.3+wip")
add_requireconfs("imgui", "imguizmo.imgui", {
    override = true, version = imgui_version, configs = imgui_configs
})

add_requires("glm 1.0.1", { configs = {
    header_only = true,
    cxx_standard = "20",
}, system = false })

add_requires("flecs-ox v4.1.0")

add_requires("fastgltf v0.8.0", { system = false, debug = is_mode("debug") })

add_requires("meshoptimizer v0.22")

local fmt_version = "11.2.0"
local fmt_configs = { header_only = false, shared = false }
add_requires("fmt " .. fmt_version, { configs = fmt_configs, system = false })

add_requires("loguru v2.1.0", { configs = {
    fmt = true,
}, system = false })
add_requireconfs("fmt", "loguru.fmt", {
    override = true,
    version = fmt_version,
    configs = fmt_configs,
    system = false
})

add_requires("vk-bootstrap v1.4.307", { system = false, debug = is_mode("debug") })

add_requires("vuk 2025.07.09", { configs = {
    debug_allocations = false,
}, debug = is_mode("debug"), private = false})
add_requireconfs("fmt", "vuk.fmt", {
    override = true,
    version = fmt_version,
    configs = fmt_configs,
    system = false
})

add_requires("shader-slang v2025.12.1", { system = false })

-- handled by system package (also nix)
add_requires("libsdl3")

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

add_requires("unordered_dense v4.5.0")

add_requires("plf_colony v7.41")

add_requires("dylib v2.2.1")

add_requires("zstd v1.5.7", { system = false })
add_requires("ktx v4.4.0", { system = false, debug = true })

add_requires("simdutf v6.2.0")

if has_config("tests") then
    add_requires("gtest", { debug = is_mode("debug"), configs = {
        main = true,
        gmock = true,
    } })
end

