target("OxylusEditor")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("Oxylus")

    add_includedirs("./src")
    add_includedirs("./vendor", { public = true })
    add_files("./src/**.cpp")

    add_files("./Resources/**")
    add_rules("ox.install_resources", {
        root_dir = os.scriptdir() .. "/Resources",
        output_dir = "Resources",
    })
    add_files("../Oxylus/src/Render/Shaders/**")
    add_rules("ox.install_shaders", {
        root_dir = "Oxylus/src/Render/Shaders",
        output_dir = "Resources/Shaders",
    })

target_end()
