# Building and editing scenes

Addressing
- Scene: every tool takes an optional `scene` (id from `editor_state`, default the active scene).
- Entity: a path `Level/Props/Crate` (exact names, no wildcards) or an id `#1234`. Every result prints `#id path` for each entity it touched. Paths survive save and load, ids survive renames. A wrong name lists the nearest siblings.
- Only entities with a TransformComponent that are not Hidden exist for MCP, the same set the hierarchy panel shows.
- Component: `TransformComponent`, `Transform`, case-insensitive, or the full flecs path. `component_types` lists them.
- Asset: a uuid or a source path like `assets_dir/Models/room.glb` or `Models/room.glb`.

Reading
- `entity_tree` for the outline (`filter`, `depth`, `limit`, `cursor`), `entity_find` by name or components, `entity_get` for full component values.

Presets for `entity_create`: empty, sprite, camera, light, sun, terrain, probe_volume, audio_source, audio_listener, particle_system. A `components` map adds a missing component and sets the given fields. A `parent` may name an entity created earlier in the same call.

Worked example, a lit room (models are placed with `entity_create_from_asset`, never by writing `model_uuid`):

1. `entity_create_from_asset` `{"asset":"Models/floor.glb","name":"Floor","transform":{"scale":[8,1,8]}}`
2. `entity_create` with
   `{"entities":[`
   `{"name":"Room","preset":"empty"},`
   `{"name":"Sun","preset":"sun","parent":"Room","components":{"Transform":{"rotation":{"euler_deg":[-50,30,0]}}}},`
   `{"name":"Camera","preset":"camera","parent":"Room","components":{"Transform":{"position":[0,2,6]}}},`
   `{"name":"Lamp","preset":"light","parent":"Room","components":{"Light":{"type":"Point","intensity":40,"color":[1,0.8,0.6]},"Transform":{"position":[0,3,0]}}}`
   `]}`
3. `entity_update` `{"entities":[{"entity":"Floor","parent":"Room"}]}` to move the floor under the room.

Changing things
- `entity_update` does name, parent (null moves to the root), enabled, remove, add, set per entity, in that order.
- `entity_duplicate` with `count` and `offset` makes a row of copies in one call.
- `entity_delete` removes children too, refs already gone with a parent are skipped.
- `component_reset` restores defaults, `component_copy` copies one component between entities.

Undo
- One tool call is one undo step labelled `mcp: <tool>`, Ctrl+Z in the editor or `editor_undo` reverts the whole call.
- If any item of a call fails, everything the call did is undone and the error says which item and operation failed.
- A scene in Play mode is not undoable. Its edits are lost at Stop and a failed call there can be partially applied, the error says so.
- With the `editor.mcp_follow` setting on, the editor selects the entity a call touched last.
