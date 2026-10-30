local managed_dir = path.join(os.scriptdir(), "..", "managed")

-- one target builds them so the test binaries never run two dotnet builds over the same intermediates
target("SmokeGameScripts")
    set_kind("phony")
    set_default(false)

    add_deps("OxylusManaged")

    add_rules("ox.csharp", {
        source_root = managed_dir,
        projects = {
            path.join(managed_dir, "SmokeGame", "SmokeGame.csproj"),
        },
        output_dir = "managed",
        aot_project = path.join(managed_dir, "SmokeGame", "SmokeGame.csproj"),
        aot_output_dir = "aot",
    })
target_end()

for _, file in ipairs(os.files(path.join(os.scriptdir(), "**Test*.cpp"))) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        set_default(false)
        set_languages("cxx23")

        add_deps("OxylusDotNet", "SmokeGameScripts")
        add_rules("ox.dotnet_nethost")

        add_files(file)

        add_tests("default", {
            runargs = { "--gtest_stack_trace_depth=10" },
        })

        add_packages("gtest")
        add_defines("OX_ENGINE_DIR=\"" .. path.translate(path.join(os.scriptdir(), "..", "..")):gsub("\\", "/") .. "\"")

        if is_plat("windows") then
            add_ldflags("/subsystem:console")
        end
end
