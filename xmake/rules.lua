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
set_extensions(".png", ".ktx", ".ktx2", ".dds", ".jpg", ".jpeg", ".mp3", ".wav", ".ogg", ".flac", ".json",
  ".otf", ".ttf", ".lua", ".txt", ".glb", ".gltf", ".oxasset", ".oxscene", ".oxparticle", ".oxterrain", ".rml", ".rcss")
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
  local config_dir  = path.directory(config_path)

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

  local root_dir = nil
  local config_text = io.readfile(config_path)
  for line in config_text:gmatch("[^\r\n]+") do
    local rd = line:match('^%s*root_directory%s*=%s*"([^"]+)"')
    if rd then
      root_dir = path.absolute(path.join(config_dir, rd))
    end
    local p = line:match('^%s*path%s*=%s*"([^"]+)"')
    if p and root_dir then
      local slang_file = path.absolute(path.join(root_dir, p))
      if os.isfile(slang_file) then
        batchcmds:add_depfiles(slang_file)
      end
    end
  end

  batchcmds:set_depmtime(os.mtime(abs_output))
  batchcmds:set_depcache(target:dependfile(abs_output))
end)

-- Cooks a game's assets at build time into `<targetdir>/<output_dir>`: the compiled packs plus the manifest
-- `AssetManager` registers them from, so a game ships without the editor ever running. `root_dir` is the asset
-- directory the game's `install_resources` copies, and `output_dir` must land where the game mounts
-- `VFS::COOKED_DIR`, `<assets>/.cooked`.
rule("ox.cook_assets")
after_build(function(target)
  import("core.project.depend")

  local root_dir = target:extraconf("rules", "ox.cook_assets", "root_dir")
  local output_dir = target:extraconf("rules", "ox.cook_assets", "output_dir") or "Assets/.cooked"
  local abs_output = path.absolute(path.join(target:targetdir(), output_dir))
  local rcli = target:dep("rcli"):targetfile()

  -- rcli skips a warm asset on its own, this only saves walking the tree when nothing moved. The file list goes in
  -- `values` too: mtimes alone never notice a file that was added or removed
  local sources = os.files(path.join(root_dir, "**"))
  table.sort(sources)
  depend.on_changed(function()
    cprint("${color.build.object}cooking assets %s -> %s", root_dir, abs_output)
    os.vrunv(rcli, { "--cook-assets", root_dir, "--output", abs_output })
  end, {
    dependfile = target:dependfile("ox.cook_assets"),
    files = table.join(sources, { rcli }),
    values = table.join({ abs_output }, sources),
    changed = not os.isfile(path.join(abs_output, "assets.oxmanifest")),
  })
end)
