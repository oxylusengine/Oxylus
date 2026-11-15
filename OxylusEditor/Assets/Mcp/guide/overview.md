# Overview of the Oxylus Editor MCP server

This server runs inside the Oxylus Editor and acts on the live editor, the same
state a person sees on screen. It is not a separate process and it does not read
project files from disk.

## Start here

Call `editor_state` first. It reports the project, the open scenes (which one is
active), the selection, visible panels, running jobs, asset scan and script
state, undo depth and the latest warnings and errors.

Call `log_read` to see what the editor logged. Pass `since_id` with the
`last_id` of the previous call to read only new lines.

## Addressing entities

An entity is addressed by its path, like `Level/Props/Crate`, or by its id,
like `#1234`. Paths survive saving and loading, ids survive renames. Results
carry both.

## Files

Move and rename project files with the `file_*` tools, not with a shell, so the
asset sidecars and the asset registry follow the file.

## Scenes and play

Use `scene_list` for the open scenes and `scene_play` to run one. Read the
`play-and-debug` guide before debugging a running scene.

## More topics

Call `editor_guide` without a topic to list the available guides.
