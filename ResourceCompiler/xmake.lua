target("ResourceCompiler")
  set_kind("shared")
  set_languages("cxx23")

  add_includedirs("./public", { public = true })
  add_includedirs("./private", { public = false })
  add_files("./private/**.cpp")
  remove_files("./private/cli.cpp")

  add_deps("Oxylus", { public = false })

  add_defines("OXRC_EXPORTS=1", { public = false })

  add_packages(
    "shader-slang",
    "simdjson-ox",
    "fastgltf-ox",
    "meshoptimizer",
    { public = false })

target_end()

target("rcli")
  -- prevent any target depends on rcli
  -- trying to compile before this one builds first
  set_policy("build.fence", true)

  set_kind("binary")
  set_languages("cxx23")
  add_files("./private/cli.cpp")

  add_deps("ResourceCompiler")

target_end()
