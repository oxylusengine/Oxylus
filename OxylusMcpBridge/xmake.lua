target("oxmcp")
  set_kind("binary")
  set_languages("cxx23")
  add_rules("ox.platform_defines")

  add_files("./src/*.cpp")
  -- header-only engine utilities, the bridge never links the engine
  add_includedirs("../Oxylus/include")
  add_packages("cpp-httplib", "simdjson")

  if is_plat("windows") then
    add_syslinks("ws2_32")
  end

  set_prefixdir("/", { bindir = ".", libdir = "." })
target_end()
