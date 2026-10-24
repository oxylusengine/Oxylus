function generate(target)
  import("core.base.semver")
  import("core.project.config")

  local version = target:version()
  local parsed_version = semver.new(version)
  local source_root = path.absolute(path.join(target:scriptdir(), ".."))
  local revision = "unknown"
  local dirty = false

  -- archives can live inside another git checkout, so only inspect the engine's own repository
  if os.exists(path.join(source_root, ".git")) then
    local revision_output = try { function ()
      return os.iorunv("git", { "-C", source_root, "rev-parse", "--verify", "HEAD" })
    end }
    local status_output = try { function ()
      return os.iorunv("git", {
        "--no-optional-locks", "-C", source_root, "status", "--porcelain=v1", "--untracked-files=normal",
      })
    end }
    if revision_output and status_output and revision_output:trim():match("^[0-9a-f]+$") then
      revision = revision_output:trim()
      dirty = #status_output:trim() > 0
    end
  end

  local build_revision = revision == "unknown" and "unknown" or ("g" .. revision:sub(1, 12))
  local full_version = version .. (version:find("+", 1, true) and "." or "+") .. build_revision
      .. (dirty and ".dirty" or "")
  local contents = string.format([[#pragma once

#define OX_VERSION_MAJOR %d
#define OX_VERSION_MINOR %d
#define OX_VERSION_PATCH %d
#define OX_VERSION "%s"
#define OX_VERSION_FULL "%s"
#define OX_VERSION_REVISION "%s"
#define OX_VERSION_BUILD_MODE "%s"
#define OX_VERSION_DIRTY %s
]], parsed_version:major(), parsed_version:minor(), parsed_version:patch(), version, full_version, revision,
      config.mode(), tostring(dirty))

  -- keep this header private to Version.cpp and leave its mtime alone when nothing changed
  local output = path.join(target:autogendir(), "version", "OxylusBuildInfo.hpp")
  if not os.isfile(output) or io.readfile(output) ~= contents then
    os.mkdir(path.directory(output))
    io.writefile(output, contents)
  end
end
