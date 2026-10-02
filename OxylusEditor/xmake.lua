local imgui_version = "v1.92.9b-docking"

add_requires("imguizmo-ox 1.84+wip.1")
add_requireconfs("imguizmo-ox.imgui", {
    override = true, version = imgui_version, configs = { wchar32 = true }
})

add_requires("imgui-node-editor-ox 0.9.4+wip")
add_requireconfs("imgui-node-editor-ox.imgui", {
    override = true, version = imgui_version, configs = { wchar32 = true }
})

add_requires("implot-ox v1.0", {
    configs = { wchar32 = true, imgui_version = imgui_version }
})

target("OxylusEditor")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("Oxylus")
    add_deps("ResourceCompiler")
    add_deps("rcli")

    add_packages("imguizmo-ox")
    add_packages("imgui-node-editor-ox")
    add_packages("implot-ox")

    add_includedirs("./src")
    add_sysincludedirs("./vendor", { public = true })
    add_files("./src/**.cpp")
    add_defines("IMGUI_DEFINE_MATH_OPERATORS")

    add_files("./Assets/**|.DS_Store|**/.DS_Store")
    add_rules("ox.install_resources", {
        root_dir = os.scriptdir() .. "/Assets",
        output_dir = "Assets",
    })
    add_files("./Assets/*.toml")
    add_rules("ox.compile_shaders", {
        output_dir = "Assets/Shaders",
    })

    set_prefixdir("/", { bindir = ".", libdir = "." })

    -- slang loads its modules at runtime, so xmake never sees them as dependencies
    after_install(function (target)
        local installdir = target:installdir()
        os.vcp(path.join(target:targetdir(), "Assets"), installdir)

        local slang = target:dep("ResourceCompiler"):pkg("shader-slang")
        if target:is_plat("windows") then
            os.vcp(path.join(slang:installdir(), "bin", "*.dll"), installdir)
        elseif target:is_plat("macosx") then
            os.vcp(path.join(slang:installdir(), "lib", "*.dylib"), installdir, { symlink = true })
        else
            for _, lib in ipairs(os.files(path.join(slang:installdir(), "lib", "*.so*"))) do
                -- the package ships the link-time name as a full copy, nothing loads it at runtime
                if #os.files(lib .. ".*") == 0 then
                    os.vcp(lib, installdir, { symlink = true })
                end
            end
        end
    end)

target_end()
