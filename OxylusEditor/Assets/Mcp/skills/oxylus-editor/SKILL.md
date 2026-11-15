---
name: oxylus-editor
description: Use when working on an Oxylus game project or driving the Oxylus editor, for example building scenes, editing components, moving assets or running the game. Covers starting the editor and connecting to its MCP server.
---

# Oxylus editor

The editor runs an MCP server whenever it is open. This skill covers what the server cannot say when it is down. Once connected, call `editor_state` first, then `editor_guide` for workflows. The tool list is the reference.

## Start the editor on a project

- `OxylusEditor --project <file.oxproj>` opens the project at startup.
- `oxmcp --launch --project <dir>` starts the editor for you when it is not running, then proxies stdio to it.
- Add `--mcp-port <n>` to the editor to pick the first port it tries (default 7780, the next ten are tried).

With a project open, the server is up when `<project>/.oxeditor/mcp.json` exists. It holds `url`, `port`, `pid`, `token_required` and `editor_version`. The token, when one is set (`editor.mcp_token`), is never in it, pass it as `Authorization: Bearer <token>` or `OXYLUS_MCP_TOKEN` for oxmcp.

## Connect a client

Claude Code over HTTP:

    claude mcp add --transport http oxylus http://127.0.0.1:7780/mcp

Claude Code over stdio, which can start the editor itself with `--launch`:

    claude mcp add oxylus -- <path>/oxmcp --project .

Any other stdio client:

    {"mcpServers":{"oxylus":{"command":"<path>/oxmcp","args":["--project","."]}}}

`oxmcp` sits next to the editor binary. The editor's `Tools > MCP` menu copies these and can install them into a project.

## Disk or live call

- Edit on disk: C# sources and any text file the editor does not own.
- Use a live call: scenes, components, material and asset settings, anything with a `.oxasset` sidecar, and every asset move or rename (`file_*` tools keep sidecars and references intact). Live edits are undoable and the editor stays in sync.
- Do not edit `.oxscene` or `.oxasset` files by hand while the editor has them open.
