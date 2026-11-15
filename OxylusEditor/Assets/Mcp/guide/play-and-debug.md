# Playing a scene and debugging it

## Play and stop

`scene_play` starts play mode and returns once the play scene exists. A C#
build may run first, so it can take a while. `scene_stop` takes the play scene
or the edit scene it came from and returns once the play scene is gone.

A play scene is a copy of the edit scene with its own id. `scene_list` shows it
as `Play` with `source` set to the edit scene. Address it with the `scene`
argument of `entity_get`, `entity_tree`, `scene_raycast` and the rest, to read
the live runtime state.

Edits to a play scene are not undoable and vanish at Stop. Make lasting changes
in the edit scene. `editor_undo` never touches play scenes.

If `Wait for Debugger on Play` is on, `scene_play` ignores it and says so.

## Reading what happened

`log_read` lists the editor log. Note `last_id` before an action and pass it as
`since_id` afterwards to read only what the action logged. Use
`min_severity: "warn"` to cut the noise.

`editor_wait` blocks until background work is idle: `jobs`, `assets`, `scripts`
or `all`. Call it after `project_open`, `script_build`, imports and saves when
the next step needs their result.

## Console

`console_exec {command}` runs a runtime console command in the focused
viewport, for example a cvar name with a value, and returns the lines it
logged. `cvar_list` and `cvar_set` cover the same settings with types.

## Probing the world

`scene_raycast {origin, direction, max_distance}` returns the first physics
body hit as an entity, point, normal and distance. Only entities with colliders
can be hit. Run it on the play scene to check what the game sees, for example a
line of sight or the ground under a character.

## Typical loop

1. `scene_save`, then `scene_play`.
2. `entity_get` on the play scene to check state, `log_read` for errors.
3. `scene_stop`, fix the edit scene, repeat.
