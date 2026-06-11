rule("mode.dist")
on_config(function(target)
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
  ".otf", ".ttf", ".lua", ".txt", ".glb", ".gltf", ".oxasset", ".oxscene", ".rml", ".rcss")
before_buildcmd_file(function(target, batchcmds, sourcefile, opt)
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

rule("ox.compile_shaders")
set_extensions(".toml")
on_buildcmd_file(function(target, batchcmds, sourcefile, opt)
  local config_path = path.absolute(sourcefile)

  local output_dir  = target:extraconf("rules", "ox.compile_shaders", "output_dir") or ""
  local output_name = target:extraconf("rules", "ox.compile_shaders", "output_name")
      or (path.basename(sourcefile) .. ".oxpack")

  local rcli        = target:dep("rcli"):targetfile()
  local abs_output  = path.absolute(path.join(target:targetdir(), output_dir, output_name))

  local args        = { "--config", config_path, "--output", abs_output }

  batchcmds:show_progress(opt.progress,
    "${color.build.object}compiling shaders from %s -> %s",
    path.filename(config_path), output_name)
  batchcmds:mkdir(path.directory(abs_output))
  batchcmds:vrunv(rcli, args)

  batchcmds:add_depfiles(sourcefile)
  batchcmds:add_depfiles(rcli)

  batchcmds:set_depmtime(os.mtime(abs_output))
  batchcmds:set_depcache(target:dependfile(abs_output))
end)
