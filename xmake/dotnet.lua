-- locates the .NET 10 SDK and drives `dotnet` for the C# scripting targets
import("core.base.semver")
import("core.project.config")

local sdk_root_cache = nil

function rid(target)
  local cpu = (target:arch() == "arm64" or target:arch() == "aarch64") and "arm64" or "x64"
  if target:is_plat("windows") then
    return "win-" .. cpu
  elseif target:is_plat("macosx") then
    return "osx-" .. cpu
  end

  return "linux-" .. cpu
end

function sdk_root()
  if sdk_root_cache then
    return sdk_root_cache
  end

  local output = try { function () return os.iorunv("dotnet", { "--list-sdks" }) end }
  for line in (output or ""):gmatch("[^\r\n]+") do
    local sdk_dir = line:match("^10%.%S+%s+%[(.+)%]$")
    if sdk_dir then
      sdk_root_cache = path.directory(sdk_dir)
    end
  end

  if not sdk_root_cache then
    raise("C# scripting needs the .NET 10 SDK on PATH, install it or configure with --csharp=n")
  end

  return sdk_root_cache
end

-- holds nethost and the hostfxr/coreclr_delegates headers
function host_pack_dir(target)
  local id = rid(target)
  local best = nil
  for _, dir in ipairs(os.dirs(path.join(sdk_root(), "packs", "Microsoft.NETCore.App.Host." .. id, "10.*"))) do
    if not best or semver.compare(path.filename(dir), path.filename(best)) > 0 then
      best = dir
    end
  end

  if not best then
    raise("No .NET 10 host pack for %s under %s", id, sdk_root())
  end

  return path.join(best, "runtimes", id, "native")
end

function nethost_library(target)
  if target:is_plat("windows") then
    return "nethost.dll"
  elseif target:is_plat("macosx") then
    return "libnethost.dylib"
  end

  return "libnethost.so"
end

-- dotnet's obj and bin, and the component mirrors OxCSharpBindgen writes, under the build directory rather than the
-- source tree, so an oxylus package built from a local checkout never shares them with the checkout's own build
function artifacts_dir()
  return path.absolute(path.join(config.builddir(), "dotnet"))
end

function generated_components()
  return path.join(artifacts_dir(), "generated", "Components.g.cs")
end

-- MSBuild wants directories with a trailing separator
local function artifacts_properties()
  return {
    "-p:ArtifactsPath=" .. artifacts_dir() .. "/",
    "-p:OxylusGeneratedDir=" .. path.directory(generated_components()) .. "/",
  }
end

local function configuration()
  return is_mode("debug") and "Debug" or "Release"
end

-- NativeAOT finds its linker through vcvarsall, which comes back empty under xmake's environment, so on windows it is
-- handed the MSVC environment xmake already detected and told to use the tools on PATH. Elsewhere it links with a
-- `clang` it looks for on PATH, which a versioned install like clang-23 doesn't provide, so it gets the compiler this
-- build uses, and no symbol stripping, which would want an objcopy as well
local function aot_linker(target)
  if not target:is_plat("windows") then
    local cc = target:tool("cc")
    return nil, { "-p:CppCompilerAndLinker=" .. cc, "-p:StripSymbols=false" }
  end

  import("core.tool.toolchain")
  local msvc = toolchain.load("msvc", { plat = target:plat(), arch = target:arch() })
  -- detection is what fills in the environment
  if not msvc:check() then
    raise("NativeAOT needs Visual Studio's MSVC linker and none was found")
  end

  return msvc:runenvs(), { "-p:IlcUseEnvironmentalTools=true" }
end

function build(project, output_dir)
  os.vrunv("dotnet", table.join(
    { "build", project, "-c", configuration(), "-o", output_dir, "--nologo", "-v", "q" },
    artifacts_properties()))
end

-- `properties` are extra MSBuild properties, { "-p:Name=Value", ... }. Always Release: what NativeAOT produces is
-- what ships, and a Debug publish is unoptimized machine code no matter the engine's mode
function publish_aot(target, project, output_dir, properties)
  local envs, extra_args = aot_linker(target)
  -- added to this process rather than passed as `envs`: windows spells it Path, and a second PATH entry loses
  local old_envs = envs and os.addenvs(envs)
  local ok, errors = try { function ()
    os.vrunv(
      "dotnet",
      table.join(
        { "publish", project, "-c", "Release", "-r", rid(target), "-o", output_dir, "--nologo", "-v", "q" },
        artifacts_properties(),
        extra_args,
        properties or {})
    )
    return true
  end, catch { function (e) return false, e end } }

  if old_envs then
    os.setenvs(old_envs)
  end

  if not ok then
    raise(errors)
  end
end
