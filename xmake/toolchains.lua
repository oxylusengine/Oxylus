toolchain("nix-clang")
    set_kind("standalone")
    set_runtimes("c++_static", "c++_shared", "stdc++_static", "stdc++_shared")

    set_toolset("cc", "clang")
    set_toolset("cxx", "clang++")
    set_toolset("ld", "clang++")
    set_toolset("sh", "clang++")
    set_toolset("ar", "llvm-ar")
    set_toolset("strip", "llvm-strip")
    set_toolset("ranlib", "llvm-ranlib")
    set_toolset("objcopy", "llvm-objcopy")
    set_toolset("mm", "clang")
    set_toolset("mxx", "clang++")
    set_toolset("as", "clang")
    set_toolset("mrc", "llvm-rc")

    on_load(function (toolchain)
        local march = is_arch("x86_64", "x64") and "-m64" or "-m32"
        toolchain:add("cxflags", march)
        toolchain:add("mxflags", march)
        toolchain:add("asflags", march)
        toolchain:add("ldflags", march)
        toolchain:add("shflags", march)
    end)

toolchain("mac-clang")
    set_kind("standalone")
    set_runtimes("c++_static", "c++_shared", "stdc++_static", "stdc++_shared")

    on_load(function (toolchain)
        -- apple clang ships an old libc++; resolve the homebrew llvm instead
        local prefix = os.getenv("LLVM_PATH")
        if not prefix or not os.isdir(prefix) then
            for _, formula in ipairs({"llvm", "llvm@23"}) do
                local outdata = try { function () return os.iorunv("brew", {"--prefix", formula}) end }
                if outdata then
                    local candidate = outdata:trim()
                    if os.isdir(candidate) then
                        prefix = candidate
                        break
                    end
                end
            end
        end
        assert(prefix and os.isdir(prefix),
               "mac-clang: no homebrew llvm found, run `brew install llvm` or set $LLVM_PATH")

        local bindir = path.join(prefix, "bin")
        for tool, program in pairs({
            cc = "clang",
            cxx = "clang++",
            ld = "clang++",
            sh = "clang++",
            ar = "llvm-ar",
            strip = "llvm-strip",
            ranlib = "llvm-ranlib",
            objcopy = "llvm-objcopy",
            mm = "clang",
            mxx = "clang++",
            as = "clang",
        }) do
            toolchain:set("toolset", tool, path.join(bindir, program))
        end

        local libcxxdir = path.join(prefix, "lib", "c++")
        local unwinddir = path.join(prefix, "lib", "unwind")
        for _, kind in ipairs({"ldflags", "shflags"}) do
            toolchain:add(kind, "-L" .. libcxxdir)
            toolchain:add(kind, "-Wl,-rpath," .. libcxxdir)
            if os.isdir(unwinddir) then
                toolchain:add(kind, "-L" .. unwinddir)
                toolchain:add(kind, "-Wl,-rpath," .. unwinddir)
            end
        end
    end)
