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

target_end()
