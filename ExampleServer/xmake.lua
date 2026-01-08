target("ExampleServer")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("Oxylus")

    add_includedirs("./src")
    add_files("./src/**.cpp")

target_end()

