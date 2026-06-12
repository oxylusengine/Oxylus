local fmt_version = "12.1.0"
local fmt_configs = { header_only = false, shared = false }
local lua_version = "v5.4.7"

packages = {
  ["stb 2024.06.01"] = {},
  ["miniaudio 0.11.24"] = {},
  ["fastgltf-ox v0.8.0"] = { system = false, debug = is_mode("debug") },
  ["meshoptimizer v1.0.1"] = {},
  ["libsdl3 3.4.4"] = { configs = { x11 = true, wayland = false } },
  ["ktx-ox v4.4.0"] = { system = false, debug = false },
  ["zstd v1.5.7"] = { system = false },
  ["shader-slang v2025.15.1"] = { configs = { shared = true }, system = false },
  ["spirv-tools 1.4.335+0"] = { system = false },
  ["enet-ox v2.6.5"] = {
    configs = {
      test = false,
      use_more_peers = false,
    },
  },
  ["imgui 42e91c315534a15133fb08fb8108cfdd515e0912"] = { configs = { wchar32 = true } },
  ["glm 1.0.3"] = {
    configs = {
      header_only = true,
      cxx_standard = "20",
    },
    system = false,
  },
  ["flecs v4.1.5"] = {},
  ["fmt " .. fmt_version] = { configs = fmt_configs, system = false },
  ["loguru v2.1.0"] = {
    configs = {
      fmt = true,
    },
    system = false,
  },
  ["vk-bootstrap v1.4.349"] = { system = false, debug = is_mode("debug") },
  ["vuk 2025.09.14.2"] = {
    configs = {
      debug_allocations = false,
    },
    debug = is_mode("debug"),
  },
  ["toml++ v3.4.0"] = {
    configs = {
      header_only = true,
    },
  },
  ["simdjson v4.0.6"] = {},
  ["joltphysics-ox v5.5.0"] = {
    configs = {
      debug_renderer = true,
      rtti = true,
      avx = true,
      avx2 = true,
      lzcnt = true,
      sse4_1 = true,
      sse4_2 = true,
      tzcnt = true,
      enable_floating_point_exceptions = false,
    },
  },
  ["tracy v0.11.1"] = {
    configs = {
      tracy_enable = has_config("profile"),
      on_demand = true,
      callstack = true,
      callstack_inlines = false,
      code_transfer = true,
      exit = true,
      system_tracing = true,
    },
  },
  ["lua " .. lua_version] = {},
  ["sol2 c1f95a773c6f8f4fde8ca3efe872e7286afe4444"] = { configs = { includes_lua = false } },
  ["unordered_dense v4.5.0"] = {},
  ["svector v1.0.3"] = {},
  ["plf_colony v7.41"] = {},
  ["simdutf v6.2.0"] = {},
  ["rmlui 6.2"] = {
    configs = {
      shared = false,
      lua = true,
    },
    debug = is_mode("debug")
  },
  ["zpp_bits v4.7.1"] = {},
}

if has_config("tests") then
  packages["gtest"] = {
    debug = is_mode("debug"),
    system = false,
    configs = {
      main = true,
      gmock = true,
    },
  }
end

confs = {
  {
    package = "fmt",
    override = "loguru.fmt",
    configs = {
      override = true,
      version = fmt_version,
      configs = fmt_configs,
      system = false,
    },
  },

  {
    package = "fmt",
    override = "vuk.fmt",
    configs = {
      override = true,
      version = fmt_version,
      configs = fmt_configs,
      system = false,
    },
  },

  {
    package = "lua",
    override = "rmlui.lua",
    configs = {
      override = true,
      version = lua_version,
      system = false,
    }
  }
}

function require_packages()
  add_requireconfs("python", { override = true, system = true })
  for name, settings in pairs(packages) do
    add_requires(name, settings)
  end
end

function require_confs()
  for _, conf in ipairs(confs) do
    add_requireconfs(conf.package, conf.override, conf.configs)
  end
end
