target("ResourceCompiler")
  set_kind("shared")
  set_languages("cxx23")

  add_includedirs("./public", { public = true })
  add_includedirs("./private", { public = false })
  add_files("./private/**.cpp")
  remove_files("./private/cli.cpp")

  add_deps("Oxylus", { public = false })
  add_forceincludes("tracy/Tracy.hpp")

  add_defines("OXRC_EXPORTS=1", { public = false })

  add_packages(
    "shader-slang",
    "zpp_bits",
    "fastgltf-ox",
    "meshoptimizer",
    "basisu-ox",
    "glm",
    { public = false })

target_end()

target("rcli")
-- prevent any target depends on rcli
-- trying to compile before this one builds first
  set_policy("build.fence", true)

  set_kind("binary")
  set_languages("cxx23")
  add_files("./private/cli.cpp", "./private/ResourceConfig.cpp")

  add_deps("ResourceCompiler")
  add_packages("fmt", "toml++", "shader-slang", "zpp_bits", "glm", "unordered_dense")

target_end()

-- fails the build when a Render/GPU/Shared.hpp type lays out differently in C++ than in Slang
target("GPULayoutCheck")
  set_kind("object")
  set_languages("cxx23")

  add_deps("Oxylus", "rcli")
  add_files("./check/GPULayoutCheck.cpp")

  on_config(function (target)
    target:add("includedirs", path.join(target:autogendir(), "gpu_layout"))
  end)

  before_build(function (target)
    import("core.project.depend")

    local rcli = target:dep("rcli"):targetfile()
    local root = path.join(target:scriptdir(), "..")
    local module = path.join(root, "Oxylus/src/Render/Shaders/shared.slang")
    local output = path.join(target:autogendir(), "gpu_layout", "GPULayoutAsserts.inl")
    local inputs = {
      module,
      path.join(root, "Oxylus/include/Render/GPU/Shared.hpp"),
      path.join(root, "Oxylus/include/Render/GPU/Prelude.hpp"),
      rcli,
    }

    depend.on_changed(function ()
      os.vrunv(rcli, { "--gpu-layout", module, "--output", output, "--include-dir", path.join(root, "Oxylus/include") })
    end, {
      dependfile = target:dependfile(output),
      files = inputs,
      changed = target:is_rebuilt() or not os.isfile(output),
    })
  end)
target_end()
