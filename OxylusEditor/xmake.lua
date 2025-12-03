add_requires("imguizmo df1c30142e7c3fb13c171aaeb328bb338fa7aaa6")
add_requireconfs("imgui", "imguizmo.imgui", {
    override = true, version = "42e91c315534a15133fb08fb8108cfdd515e0912", configs = { wchar32 = true }
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
