-- the smallest shipped game: App with the default modules, a start scene cooked by rcli, and C# scripts published with
-- NativeAOT into one native library, so it runs on a machine with no .NET installed. A new game starts as a copy of it
local scripts_dir = path.join(os.scriptdir(), "..", "OxylusScriptingDotNet", "managed")

target("SmokeGame")
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")
    -- a folder of its own, its shader pack, cooked assets and scripts would land on the editor's in the shared one
    set_targetdir(path.join("$(builddir)", "$(plat)", "$(arch)", "$(mode)", "SmokeGame"))

    add_deps("Oxylus", "OxylusDotNet", "OxylusManaged", "rcli")
    if has_config("tests") then
        -- SmokeGameScripts publishes the same C# project for the tests, and two restores of it at once collide
        add_deps("SmokeGameScripts")
    end

    add_files("./src/*.cpp")
    add_forceincludes("tracy/Tracy.hpp")

    add_files(path.join(os.scriptdir(), "..", "OxylusEditor", "Assets", "engine.toml"))
    add_rules("ox.compile_shaders", { output_dir = "Assets/Shaders" })
    add_rules("ox.cook_assets", { root_dir = path.join(os.scriptdir(), "Assets") })
    add_rules("ox.publish_csharp", {
        project = path.join(scripts_dir, "SmokeGame", "SmokeGame.csproj"),
        source_roots = { scripts_dir },
    })

    set_prefixdir("/", { bindir = ".", libdir = "." })

    after_install(function (target)
        os.vcp(path.join(target:targetdir(), "Assets"), target:installdir())
    end)
target_end()
