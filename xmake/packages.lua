local fmt_version = "12.1.0"
local fmt_configs = { header_only = false, shared = false }
local lua_version = "v5.4.7"
local imgui_version = "v1.92.9b-docking"
local simdjson_version = "v4.2.4"
local vulkan_headers_version = "1.4.335+0"
local vma_version = "v3.1.0"
local unordered_dense_version = "v4.8.1"
local svector_version = "v1.0.3"
local tracy_version = "v0.13.1"
local tracy_configs = {
  tracy_enable = has_config("profile"),
  on_demand = true,
  callstack = true,
  callstack_inlines = false,
  code_transfer = true,
  exit = true,
  system_tracing = true,
}

packages = {
  ["miniaudio 0.11.25"] = {},
  ["fastgltf-ox v0.8.0"] = { system = false, debug = is_mode("debug") },
  ["meshoptimizer v1.2"] = {},
  ["libsdl3 3.4.12"] = { configs = { x11 = true, wayland = false } },
  -- release even in debug builds, an unoptimized BC7 encoder makes every texture import crawl
  ["basisu-ox 2.50"] = { system = false, debug = false },
  ["shader-slang v2026.12.2"] = { configs = { shared = true }, system = false },
  ["enet-ox v2.6.5"] = {
    configs = {
      test = false,
      use_more_peers = false,
    },
  },
  ["imgui " .. imgui_version] = { configs = { wchar32 = true } },
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
  ["vk-bootstrap v1.4.354"] = { system = false, debug = is_mode("debug") },
  ["vulkan-headers " .. vulkan_headers_version] = { system = false },
  ["vir 2026.10.19"] = { debug = is_mode("debug"), system = false },
  ["toml++ v3.4.0"] = {
    configs = {
      header_only = true,
    },
  },
  ["simdjson " .. simdjson_version] = { system = false },
  ["joltphysics v5.6.0"] = {
    configs = {
      debug_renderer = true,
      rtti = true,
      avx = true,
      avx2 = true,
      lzcnt = true,
      sse4_1 = true,
      sse4_2 = true,
      tzcnt = true,
    },
  },
  ["tracy " .. tracy_version] = { configs = tracy_configs },
  ["lua " .. lua_version] = {},
  ["sol2 c1f95a773c6f8f4fde8ca3efe872e7286afe4444"] = { configs = { includes_lua = false } },
  ["unordered_dense " .. unordered_dense_version] = {},
  ["svector " .. svector_version] = {},
  ["plf_colony v7.41"] = {},
  ["simdutf v8.2.0"] = {},
  ["rmlui f7b297e2c8fc44c5e85df498dbae91762c0769a5"] = {
    configs = {
      shared = false,
      lua = true,
    },
    debug = is_mode("debug")
  },
  ["zpp_bits v4.7.1"] = {},
  ["zstd v1.5.7"] = { system = false },
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
    package = "simdjson",
    override = "fastgltf-ox.simdjson",
    configs = {
      override = true,
      version = simdjson_version,
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
  },

  {
    package = "fmt",
    override = "vir.fmt",
    configs = {
      override = true,
      version = fmt_version,
      configs = fmt_configs,
      system = false,
    },
  },

  {
    package = "tracy",
    override = "vir.tracy",
    configs = {
      override = true,
      version = tracy_version,
      configs = tracy_configs,
    },
  },

  {
    package = "vulkan-headers",
    override = "vir.vulkan-headers",
    configs = {
      override = true,
      version = vulkan_headers_version,
      system = false,
    },
  },

  {
    package = "vulkan-memory-allocator",
    override = "vir.vulkan-memory-allocator",
    configs = {
      override = true,
      version = vma_version,
      system = false,
    },
  },

  {
    package = "unordered_dense",
    override = "vir.unordered_dense",
    configs = {
      override = true,
      version = unordered_dense_version,
    },
  },

  {
    package = "svector",
    override = "vir.svector",
    configs = {
      override = true,
      version = svector_version,
    },
  },
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
