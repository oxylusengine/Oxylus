rule("mode.dist")
    on_config(function (target)
        if is_mode("dist") then
            if not target:get("symbols") and target:kind() ~= "shared" then
                target:set("symbols", "hidden")
            end

            if not target:get("optimize") then
                if target:is_plat("android", "iphoneos") then
                    target:set("optimize", "smallest")
                else
                    target:set("optimize", "fastest")
                end
            end

            if not target:get("strip") then
                target:set("strip", "all")
            end

            target:add("cxflags", "-DNDEBUG")
            target:add("cuflags", "-DNDEBUG")
        end
    end)

rule("ox.install_resources")
    set_extensions(".png", ".ktx", ".ktx2", ".dds", ".jpg", ".mp3", ".wav", ".ogg",
    ".otf", ".ttf", ".lua", ".txt", ".glb", ".gltf", ".oxasset", ".oxscene")
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

rule("ox.install_shaders")
    set_extensions(".slang", ".hlsl", ".hlsli", ".frag", ".vert", ".comp", ".h", ".json")
    before_buildcmd_files(function (target, batchcmds, sourcebatch, opt)
        local output_dir = target:extraconf("rules", "ox.install_shaders", "output_dir") or ""

        for _, sourcefile in ipairs(sourcebatch.sourcefiles) do
            local abs_source = path.absolute(sourcefile)
            local source_dir = path.directory(abs_source)

            -- Find the "Shaders" directory in the path and extract everything after it
            local shaders_pattern = "[/\\]Shaders[/\\]"
            local shaders_start, shaders_end = source_dir:find(shaders_pattern)

            local rel_output = path.join(target:targetdir(), output_dir)

            if shaders_start then
                -- Get the part after "/Shaders/"
                local subpath = source_dir:sub(shaders_end + 1)
                if subpath and subpath ~= "" then
                    rel_output = path.join(rel_output, subpath)
                end
            end

            local abs_output = path.join(rel_output, path.filename(sourcefile))
            batchcmds:show_progress(opt.progress, "${color.build.object}copying shader file %s", sourcefile)
            batchcmds:mkdir(path.directory(abs_output))
            batchcmds:cp(abs_source, abs_output)

            batchcmds:add_depfiles(sourcefile)
            batchcmds:set_depmtime(os.mtime(abs_output))
            batchcmds:set_depcache(target:dependfile(abs_output))
        end
    end)


