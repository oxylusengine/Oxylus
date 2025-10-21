add_requires("imguizmo 1.91.3+wip")
add_requireconfs("imgui", "imguizmo.imgui", {
    override = true, version = "v1.92.0-docking", configs = { wchar32 = true }
})

target("OxylusEditor")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("Oxylus")
    add_packages("imguizmo")

    add_includedirs("./src")
    add_sysincludedirs("./vendor", { public = true })
    add_files("./src/**.cpp")

    add_files("./Resources/**")
    add_rules("ox.install_resources", {
        root_dir = os.scriptdir() .. "/Resources",
        output_dir = "Resources",
    })
    add_files("../Oxylus/src/Render/Shaders/**")
    add_rules("ox.install_shaders", {
        output_dir = "Resources/Shaders",
    })

target_end()
