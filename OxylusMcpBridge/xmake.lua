target("oxmcp")
  set_kind("binary")
  set_languages("cxx23")

  add_files("./src/*.cpp")
  add_packages("cpp-httplib", "simdjson")

  if is_plat("windows") then
    add_syslinks("ws2_32")
  end

  set_prefixdir("/", { bindir = ".", libdir = "." })
target_end()
