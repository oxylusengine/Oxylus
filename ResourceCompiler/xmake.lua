target("ResourceCompiler")
  set_kind("shared")
  set_languages("cxx23")

  add_includedirs("./public", { public = true })
  add_includedirs("./private", { public = false })
  add_files("./private/**.cpp")

  add_deps("Oxylus", { public = false })

  add_defines("OXRC_EXPORTS=1", { public = false })

  add_packages("shader-slang", { public = false })

target_end()
