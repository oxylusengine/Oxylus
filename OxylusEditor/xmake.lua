local imgui_version = "v1.92.9b-docking"

add_requires("imguizmo df1c30142e7c3fb13c171aaeb328bb338fa7aaa6")
add_requireconfs("imguizmo.imgui", {
    override = true, version = imgui_version, configs = { wchar32 = true }
})

target("OxylusEditor")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("Oxylus")
    add_deps("ResourceCompiler")
    add_deps("rcli")
    add_packages("imguizmo")

    add_includedirs("./src")
    add_sysincludedirs("./vendor", { public = true })
    add_files("./src/**.cpp")

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
