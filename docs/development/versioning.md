# Versioning

## Release versions

The engine release version is declared once in the root [xmake.lua](../../xmake.lua), starting at `0.1.0`.
Releases use [Semantic Versioning](https://semver.org/). During `0.x` development, bump the minor
version for features or breaking C++/C# API changes, and the patch version for compatible fixes.
From `1.0.0`, breaking API changes require a major version bump.

## Build identity

Every game and the editor log the linked engine's build information automatically during startup:

```text
Oxylus 0.1.0+g8c42b2c25179.dirty (debug)
```

The build system embeds the full Git commit hash, a `.dirty` marker for tracked or untracked changes
(excluding ignored files), and the build mode. The displayed hash is shortened to 12 characters.
Metadata is checked on every engine build; the private generated header is only rewritten when it
changes, so an unchanged build stays incremental. No Git commands run at application startup.
Without the engine's own Git metadata or a working Git executable, the revision is `unknown` and the
version is printed as `0.1.0+unknown`; dirty status is unavailable in that case.

In the editor, **Help > About** shows the same build information alongside the platform, GPU and
supported Vulkan API version. Use **Copy build info** to copy these details, including the full
revision and source tree status, for a bug report.

## Runtime API

Game code can inspect the same information without creating an `App`:

```cpp
#include "Core/Version.hpp"

const auto& build = ox::engine_build_info();
// build.version: release version, including any prerelease label
// build.full_version: release version plus source revision and dirty marker
// build.revision: full commit hash, or "unknown"
// build.major, build.minor, build.patch, build.build_mode, build.dirty
```

This function is compiled into the engine library, so it reports the engine that was actually linked.
The game has its own release version, independent of the engine. Asset format and compiler versions
also remain independent and continue to control compatibility and recooking.

## Releases

For a release, set the version in [xmake.lua](../../xmake.lua), update [CHANGELOG.md](../../CHANGELOG.md),
and tag the release commit `v0.1.0` (or the corresponding version). Published versions and tags must
remain unchanged. Start the next development cycle by setting a prerelease version such as
`0.2.0-dev`; alpha, beta and release candidates can use labels such as `0.2.0-alpha.1` or
`0.2.0-rc.1`. Build metadata after `+` does not affect version ordering. Pin each game's engine
dependency to a release tag or commit to make upgrades deliberate; the startup log identifies
stale binaries but does not check for updates.
