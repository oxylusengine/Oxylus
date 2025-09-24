target("Oxylus")
    set_kind("static")
    set_languages("cxx23")
    add_rpathdirs("@executable_path")

    add_includedirs("./include", { public = true })
    add_files("./src/**.cpp")
    add_forceincludes("tracy/Tracy.hpp")

    add_options("profile")
    add_options("llvmpipe")
    if not has_config("lua_bindings") then
        remove_files("./src/Scripting/*Bindings*")
    else
        add_defines("OX_LUA_BINDINGS")
    end

    if is_plat("windows") then
        add_defines("_UNICODE", { force = true, public = true  })
        add_defines("UNICODE", { force = true, public = true  })
        add_defines("WIN32_LEAN_AND_MEAN", { force = true, public = true  })
        add_defines("VC_EXTRALEAN", { force = true, public = true  })
        add_defines("NOMINMAX", { force = true, public = true  })
        add_defines("_WIN32", { force = true, public = true  })
        add_defines("_CRT_SECURE_NO_WARNINGS", { force = true, public = true  })

        remove_files("./src/OS/Linux*")
        remove_files("./src/OS/MacOS*")

        add_defines("OX_PLATFORM_WINDOWS", { public = true })
    elseif is_plat("linux") then
        remove_files("./src/OS/Win32*")
        remove_files("./src/OS/MacOS*")

        add_defines("OX_PLATFORM_LINUX", { public = true })
    elseif is_plat("macosx") then
        remove_files("./src/OS/Win32*")
        remove_files("./src/OS/Linux*")

        add_defines("OX_PLATFORM_MACOSX", { public = true })
    end

    if is_mode("debug")  then
        add_defines("OX_DEBUG", { public = true })
        add_defines("_DEBUG", { public = true })
    elseif is_mode("release") or is_mode("releasedbg") then
        add_defines("OX_RELEASE", { public = true })
        add_defines("NDEBUG", { public = true })
    elseif is_mode("dist") then
        add_defines("OX_DISTRIBUTION", { public = true })
        add_defines("NDEBUG", { public = true })
    end

    -- Library defs
    add_defines(
        "GLM_ENABLE_EXPERIMENTAL",
        "GLM_FORCE_DEPTH_ZERO_TO_ONE",
        { public = true })

    on_config(function (target)
        if (target:has_tool("cxx", "msvc", "cl")) then
            target:add("defines", "OX_COMPILER_MSVC=1", { force = true, public = true })
        elseif(target:has_tool("cxx", "clang", "clangxx")) then
            target:add("defines", "OX_COMPILER_CLANG=1", { force = true, public = true })
        elseif target:has_tool("cxx", "gcc", "gxx") then
            target:add("defines", "OX_COMPILER_GCC=1", { force = true, public = true })
        end
    end)

    add_cxxflags(
        "/permissive-",
        "/EHsc",
        "/bigobj",
        "-wd4100",
        "/Zc:preprocessor",
        { public = true, tools = { "msvc", "cl", "clang_cl", "clang-cl" } })

    add_packages(
        "stb",
        "miniaudio",
        "imgui",
        "imguizmo",
        "glm",
        "flecs-ox",
        "fastgltf-ox",
        "meshoptimizer",
        "fmt",
        "loguru",
        "vk-bootstrap",
        "vuk",
        "libsdl3",
        "toml++",
        "joltphysics-ox",
        "tracy",
        "sol2",
        "unordered_dense",
        "dylib",
        "ktx-ox",
        "simdutf",
        "plf_colony",
        "shader-slang",
        { public = true })

target_end()
