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
  ".otf", ".ttf", ".txt", ".glb", ".gltf", ".oxasset", ".oxscene", ".oxparticle", ".oxcine", ".oxterrain", ".rml", ".rcss")
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

  if not has_config("compile_resources") then
    return
  end

  import("private.action.run.runenvs")

  local rcli_target = target:dep("rcli")
  local rcli        = rcli_target:targetfile()
  -- windows has no rpath, rcli finds slang's dlls through the package PATH
  local rcli_envs   = runenvs.join(runenvs.make(rcli_target))
  local abs_output  = path.absolute(path.join(target:targetdir(), output_dir, output_name))

  local args        = { "--config", config_path, "--output", abs_output }
  -- shader debug info is for RenderDoc and Nsight, release and dist ship without it
  if is_mode("debug") then
    table.insert(args, "--debug-symbols")
  end

  batchcmds:show_progress(opt.progress,
    "${color.build.object}compiling shaders from %s -> %s",
    path.filename(config_path), output_name)
  batchcmds:mkdir(path.directory(abs_output))
  batchcmds:vrunv(rcli, args, { envs = rcli_envs })

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

  -- shared.slang textually includes these, so an edit to a GPU struct has to recompile the pack
  for _, shared_header in ipairs(os.files(path.join(os.scriptdir(), "../Oxylus/include/Render/GPU/*.hpp"))) do
    batchcmds:add_depfiles(shared_header)
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
  if not has_config("compile_resources") then
    return
  end

  import("core.project.depend")
  import("private.action.run.runenvs")

  local root_dir = target:extraconf("rules", "ox.cook_assets", "root_dir")
  local output_dir = target:extraconf("rules", "ox.cook_assets", "output_dir") or "Assets/.cooked"
  local abs_output = path.absolute(path.join(target:targetdir(), output_dir))
  local rcli_target = target:dep("rcli")
  local rcli = rcli_target:targetfile()
  -- windows has no rpath, rcli finds slang's dlls through the package PATH
  local rcli_envs = runenvs.join(runenvs.make(rcli_target))

  -- rcli skips a warm asset on its own, this only saves walking the tree when nothing moved. The file list goes in
  -- `values` too: mtimes alone never notice a file that was added or removed
  local sources = os.files(path.join(root_dir, "**"))
  table.sort(sources)
  depend.on_changed(function()
    cprint("${color.build.object}cooking assets %s -> %s", root_dir, abs_output)
    os.vrunv(rcli, { "--cook-assets", root_dir, "--output", abs_output }, { envs = rcli_envs })
  end, {
    dependfile = target:dependfile("ox.cook_assets"),
    files = table.join(sources, { rcli }),
    values = table.join({ abs_output }, sources),
    changed = not os.isfile(path.join(abs_output, "assets.oxmanifest")),
  })
end)

-- Builds C# projects for the hostfxr host with `dotnet build`, each of `projects` in order into
-- `<targetdir>/<output_dir>`, and publishes `aot_project` with NativeAOT into `<targetdir>/<aot_output_dir>`.
-- `source_root` is walked for the sources that decide whether anything needs rebuilding, and `bindgen` names the
-- dependency that writes the component mirrors first
rule("ox.csharp")
after_build(function(target)
  import("core.project.depend")
  import("dotnet", { rootdir = os.scriptdir() })

  local source_root = target:extraconf("rules", "ox.csharp", "source_root")
  local projects = target:extraconf("rules", "ox.csharp", "projects") or {}
  local output_dir = path.absolute(path.join(target:targetdir(),
    target:extraconf("rules", "ox.csharp", "output_dir") or "managed"))
  local aot_project = target:extraconf("rules", "ox.csharp", "aot_project")
  local aot_output_dir = path.absolute(path.join(target:targetdir(),
    target:extraconf("rules", "ox.csharp", "aot_output_dir") or "aot"))

  -- the component mirrors follow the engine, so they are regenerated before anything is decided
  local generated = dotnet.generated_components()
  local bindgen = target:extraconf("rules", "ox.csharp", "bindgen")
  if bindgen then
    import("private.action.run.runenvs")
    local bindgen_target = target:dep(bindgen)
    os.vrunv(bindgen_target:targetfile(), { generated }, { envs = runenvs.join(runenvs.make(bindgen_target)) })
  end

  local sources = table.join(
    { generated },
    os.files(path.join(source_root, "**.cs")),
    os.files(path.join(source_root, "**.csproj")),
    os.files(path.join(source_root, "**.props")))
  table.sort(sources)

  local outputs = { output_dir }
  for _, project in ipairs(projects) do
    table.insert(outputs, path.join(output_dir, path.basename(project) .. ".dll"))
  end

  if aot_project then
    table.insert(outputs, aot_output_dir)
  end

  depend.on_changed(function()
    for _, project in ipairs(projects) do
      cprint("${color.build.object}building C# %s", path.filename(project))
      dotnet.build(project, output_dir)
    end

    if aot_project then
      cprint("${color.build.object}publishing C# %s with NativeAOT", path.filename(aot_project))
      dotnet.publish_aot(target, aot_project, aot_output_dir)
    end
  end, {
    dependfile = target:dependfile("ox.csharp"),
    files = sources,
    values = table.join(outputs, sources),
    changed = not os.isdir(output_dir) or (aot_project and not os.isdir(aot_output_dir)),
  })
end)

-- Publishes a game's C# scripts with NativeAOT into `<targetdir>/<output_dir>` (default "scripts") and installs them
-- next to the game, one native library with no .NET runtime behind it. NativeAOT can't cross compile between operating
-- systems, so each platform publishes its own. `source_roots` are walked for the C# that decides whether to publish
-- again, the generated component mirrors always count. The target depends on OxylusManaged, which writes those
rule("ox.publish_csharp")
after_build(function(target)
  import("core.project.depend")
  import("dotnet", { rootdir = os.scriptdir() })

  local project = target:extraconf("rules", "ox.publish_csharp", "project")
  local output_dir = path.absolute(path.join(target:targetdir(),
    target:extraconf("rules", "ox.publish_csharp", "output_dir") or "scripts"))
  local source_roots = target:extraconf("rules", "ox.publish_csharp", "source_roots") or { path.directory(project) }

  local sources = { dotnet.generated_components() }
  for _, root in ipairs(source_roots) do
    table.join2(sources, os.files(path.join(root, "**.cs")), os.files(path.join(root, "**.csproj")),
      os.files(path.join(root, "**.props")))
  end
  table.sort(sources)

  depend.on_changed(function()
    cprint("${color.build.object}publishing C# %s with NativeAOT", path.filename(project))
    dotnet.publish_aot(target, project, output_dir, {
      -- a game needs no culture data or exception message resources, and trades size for speed
      "-p:InvariantGlobalization=true",
      "-p:UseSystemResourceKeys=true",
      "-p:OptimizationPreference=Speed",
    })
  end, {
    dependfile = target:dependfile("ox.publish_csharp"),
    files = sources,
    values = table.join({ output_dir }, sources),
    changed = not os.isdir(output_dir),
  })
end)
after_install(function(target)
  local output_dir = target:extraconf("rules", "ox.publish_csharp", "output_dir") or "scripts"
  local published = path.join(target:targetdir(), output_dir)
  local destination = path.join(target:installdir(), output_dir)
  os.mkdir(destination)
  -- the library alone, the debug symbols and import library the publish leaves next to it don't ship
  local libraries = table.join(os.files(path.join(published, "*.dll")), os.files(path.join(published, "*.so")),
    os.files(path.join(published, "*.dylib")))
  for _, library in ipairs(libraries) do
    os.vcp(library, destination)
  end
end)

-- Copies nethost next to the binary, the hostfxr host loads it from there to find the installed runtime
rule("ox.dotnet_nethost")
after_build(function(target)
  import("dotnet", { rootdir = os.scriptdir() })
  local library = path.join(dotnet.host_pack_dir(target), dotnet.nethost_library(target))
  local destination = path.join(target:targetdir(), path.filename(library))
  if not os.isfile(destination) or os.mtime(destination) < os.mtime(library) then
    os.vcp(library, destination)
  end
end)
after_install(function(target)
  import("dotnet", { rootdir = os.scriptdir() })
  os.vcp(path.join(dotnet.host_pack_dir(target), dotnet.nethost_library(target)), target:installdir())
end)
