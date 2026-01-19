target("Pong")
    set_kind("binary")
    set_languages("cxx23")
    add_rpathdirs("@executable_path")

    add_includedirs(".")
    add_files("./src/**.cpp")

    add_deps("Oxylus")

    add_files("./Assets/**")
    add_rules("ox.install_resources", {
        root_dir = os.scriptdir() .. "/Assets",
        output_dir = "Resources",
    })
    add_rules("ox.install_shaders", {
        output_dir = "Resources/Shaders",
    })
target_end()
