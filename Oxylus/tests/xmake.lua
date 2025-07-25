for _, file in ipairs(os.files("./**/Test*.cpp")) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        set_default(false)
        set_languages("cxx23")

        add_deps("Oxylus")
        add_forceincludes("Tracy.hpp")

        add_files(file)

        add_tests("default", { runargs = { "--gmock_verbose=info", "--gtest_stack_trace_depth=10" } })

        add_packages("gtest")

        if is_plat("windows") then
            add_ldflags("/subsystem:console")
        end
end