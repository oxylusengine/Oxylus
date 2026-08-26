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

    add_files("./Assets/**")
    add_rules("ox.install_resources", {
        root_dir = os.scriptdir() .. "/Assets",
        output_dir = "Assets",
    })
    add_files("./Assets/*.toml")
    add_rules("ox.compile_shaders", {
        output_dir = "Assets/Shaders",
    })

target_end()
