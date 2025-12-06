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

rule("ox.copy_resources")
    set_extensions(".png", ".ktx", ".ktx2", ".dds", ".jpg", ".mp3", ".wav", ".ogg",
    ".otf", ".ttf", ".lua", ".txt", ".glb", ".gltf", ".oxasset", ".oxscene")
    before_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local output_dir = target:extraconf("rules", "ox.copy_resources", "output_dir") or ""
        local root_dir = target:extraconf("rules", "ox.copy_resources", "root_dir") or os.scriptdir()

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

rule("ox.compile_resources")
    set_extensions(".rcm")
    before_build_file(function(target, sourcefile, opt)
        import("core.base.task")
        import("utils.progress")

        local sourcebasename = path.basename(sourcefile)
        local source_rel = path.relative(sourcefile, target:scriptdir())
        local outdir = path.join(target:autogendir(), path.directory(source_rel))
        local outfile = path.join(outdir, sourcebasename .. ".bin")
        local cachefile = path.join(outdir, sourcebasename .. ".cache.json")

        progress.show(opt.progress, "${color.build.object}compiling.resources %s", source_rel)
        os.mkdir(outdir)
        local meta_abs = path.absolute(sourcefile)
        local cache_abs = path.absolute(cachefile)

        os.vrunv(target:dep("rcli"):targetfile(), {
            "--meta", meta_abs,
            "--cache", cache_abs
        })

        local generated_bin = path.join(path.directory(sourcefile), sourcebasename .. ".bin")
        -- if os.exists(generated_bin) then
        --     os.cp(generated_bin, outfile)
        -- end
    end)
