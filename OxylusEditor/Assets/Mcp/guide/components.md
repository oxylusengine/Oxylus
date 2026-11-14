# Reading and writing component fields

`component_types` without a name lists every component. With a name it returns the schema: fields in order with `type`, enum `constants`, `default` and, for uuid fields, `asset_type`. Nested structs appear as `struct` with their own `fields`.

Reading (`entity_get`)
- `{"id":"#12","path":"Room/Lamp","enabled":true,"parent":"#10 Room","children":[],"components":{"TransformComponent":{"position":{"x":0,"y":3,"z":0},"rotation":{"x":0,"y":0,"z":0,"w":1,"euler_deg":[0,0,0]},"scale":{"x":1,"y":1,"z":1}}},"tags":[]}`
- Floats print in full precision, non-finite values as null. Enums print as constant names, uuid fields as the uuid (empty for none), entity fields as `#id path`.
- `assets` maps every referenced uuid to `{"type","path"}`.
- Array, vector and bitmask fields are not shown.

Writing is a patch: only the keys you send change. Unknown keys, wrong types and out of range numbers are errors that name the field path (`TransformComponent.positon: unknown field, valid: position, rotation, scale`).
- Numbers: integers are fine for float fields, floats only for integer fields when integral. Booleans are true or false.
- Vectors: `{"x":1,"y":2,"z":3}` or `[1,2,3]` (also for vec2, vec4, ivec2, uvec3). A partial object changes only the given axes.
- Quaternions: `{"x":0,"y":0,"z":0,"w":1}` (normalized afterwards), `[x,y,z,w]`, or `{"euler_deg":[x,y,z]}` / `{"euler_deg":{"x":..,"y":..,"z":..}}`. Do not mix the two forms.
- Enums: the constant name, case-insensitive (`"Point"`), or an integer that is a valid constant. The error lists the constants.
- UUID fields: an asset path or uuid string, `""` or null clears it. A mismatch with the type the field name suggests is a warning, not an error. `MeshComponent.model_uuid` is refused, use `entity_create_from_asset`.
- Entity fields: a path or `#id`, null clears.
- Strings: plain strings.

Example `entity_update` item: `{"entity":"Room/Lamp","set":{"Light":{"intensity":60}},"add":["AudioSource"]}`. Components named in `set` must exist, so put new ones in `add` of the same item or use `entity_create` with `components`.

Asset reference counts follow the edit automatically, including on undo and redo.
