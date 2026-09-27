-- scans OX_COMPONENT structs and emits their flecs/Lua registration, see the ox.components rule
target("ecsgen")
  set_policy("build.fence", true)

  set_kind("binary")
  set_languages("cxx23")
  add_files("./main.cpp")
  -- header-only Core/Types.hpp, ecsgen must not link Oxylus since Oxylus depends on it
  add_includedirs("../../Oxylus/include")

  add_tests("golden")
  on_test(function(target, opt)
    local tests = path.join(target:scriptdir(), "tests")
    local output = os.iorunv(target:targetfile(), {
      "--input", path.join(tests, "Fixture.hpp"),
      "--function", "bind_fixture_components",
      "--output", "-",
    })
    local expected = io.readfile(path.join(tests, "Fixture.expected.inl"))
    if output ~= expected then
      print(output)
      return false
    end
    return true
  end)
target_end()
