for _, file in ipairs(os.files("./**/Test*.cpp")) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        set_default(false)
        set_languages("cxx23")

        add_deps("Oxylus")

        add_files(file)

        add_tests("default", { runargs = { "--gmock_verbose=info", "--gtest_stack_trace_depth=10" } })

        add_packages("gtest")

        if is_plat("linux", "macosx") then
            set_policy("build.sanitizer.address", true)
            set_policy("build.sanitizer.undefined", true)
            if is_plat("linux") then
                set_policy("build.sanitizer.leak", true)
            end
        elseif is_plat("windows") then
            -- ASAN is available on Windows with recent MSVC and Clang(only on debug builds)
            -- Enable it and let xmake handle toolchain compatibility
            if not is_mode("debug") then
                set_policy("build.sanitizer.address", true)
            end
        end

        if is_plat("windows") then
            add_ldflags("/subsystem:console")
        end
end