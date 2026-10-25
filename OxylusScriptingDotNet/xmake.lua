target("OxylusDotNet")
    set_kind("static")
    set_languages("cxx23")

    add_deps("Oxylus")

    add_includedirs("./include", { public = true })
    add_files("./src/**.cpp")
    add_forceincludes("tracy/Tracy.hpp")
    add_defines("OX_DOTNET", { public = true })

    on_load(function (target)
        import("dotnet", { rootdir = path.join(os.scriptdir(), "..", "xmake") })
        target:add("sysincludedirs", dotnet.host_pack_dir(target))
    end)
target_end()

-- writes build/dotnet/generated/Components.g.cs, the C# mirrors of the engine's components
target("OxCSharpBindgen")
    set_kind("binary")
    set_default(false)
    set_languages("cxx23")

    add_deps("Oxylus")
    add_files("./bindgen/*.cpp")
    add_forceincludes("tracy/Tracy.hpp")

    if is_plat("windows") then
        add_ldflags("/subsystem:console")
    end
target_end()

-- builds Oxylus.Scripting and the generator into <targetdir>/managed, the one dotnet build of them, so targets that
-- need them depend on this rather than building them over the same intermediates at the same time
target("OxylusManaged")
    set_kind("phony")
    set_default(false)

    add_deps("OxCSharpBindgen")

    local managed_dir = path.join(os.scriptdir(), "managed")
    add_rules("ox.csharp", {
        source_root = managed_dir,
        bindgen = "OxCSharpBindgen",
        projects = {
            path.join(managed_dir, "Oxylus.Scripting", "Oxylus.Scripting.csproj"),
            path.join(managed_dir, "Oxylus.Scripting.Generators", "Oxylus.Scripting.Generators.csproj"),
        },
        output_dir = "managed",
    })
target_end()

if has_config("tests") then
    includes("tests")
end
