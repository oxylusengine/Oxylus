rule("mode.dist")
    on_config(function (target)

        -- is release mode now? xmake f -m release
        if is_mode("dist") then

            -- set the symbols visibility: hidden
            if not target:get("symbols") and target:kind() ~= "shared" then
                target:set("symbols", "hidden")
            end

            -- enable optimization
            if not target:get("optimize") then
                if target:is_plat("android", "iphoneos") then
                    target:set("optimize", "smallest")
                else
                    target:set("optimize", "fastest")
                end
            end

            -- strip all symbols
            if not target:get("strip") then
                target:set("strip", "all")
            end

            -- enable NDEBUG macros to disables standard-C assertions
            target:add("cxflags", "-DNDEBUG")
            target:add("cuflags", "-DNDEBUG")
        end
    end)

rule("ox.install_resources")
    set_extensions(
        ".png", ".ttf", ".slang", ".lua", ".txt", ".glb",
        ".hlsl", ".hlsli", ".frag", ".vert", ".comp", ".h")
    before_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local output_dir = target:extraconf("rules", "ox.install_resources", "output_dir") or ""
        local root_dir = target:extraconf("rules", "ox.install_resources", "root_dir") or os.scriptdir()

        local abs_source = path.absolute(sourcefile)
        local rel_output = path.join(target:targetdir(), output_dir)
        if (root_dir ~= "" or root_dir ~= nil) then
            local rel_root = path.relative(path.directory(abs_source), root_dir)
            rel_output = path.join(rel_output, rel_root)
        end

        local abs_output = path.absolute(rel_output) .. "/" .. path.filename(sourcefile)
        batchcmds:show_progress(opt.progress, "${color.build.object}copying resource file %s", sourcefile)
        batchcmds:cp(abs_source, abs_output)

        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(abs_output))
        batchcmds:set_depcache(target:dependfile(abs_output))
    end)

