# Assets, files and materials

Referencing assets
- An asset is a uuid. Everywhere a tool takes `asset` you can pass the uuid or the path of its file (`Assets/Props/crate.glb`, absolute, or `assets_dir/Props/crate.glb`). A material or particle system created in the editor is addressed by the path you created it with, `.oxasset` is optional.
- `asset_list` filters by `type`, `path_prefix` and `loaded_only`. A model's materials and textures have no file, they show as `model path#slot name`, so take their uuid from `asset_get` on the model.
- Component fields like `MeshComponent.material_uuid` take a uuid or an asset path in `entity_update`.

Moving files
- Never move, rename or delete asset files with a shell. `file_move`, `file_rename` and `file_delete` carry the `.oxasset` sidecar and the registry with the file, so uuids and every reference to them survive. `file_delete` refuses while a loaded asset under the path is still in use, `force` deletes the files anyway, assets still in use then stay registered until the editor restarts.
- `file_list` shows `file size Type uuid`, `file_mkdir` creates folders, `file_open` does what a double-click does.

Importing
- `asset_import {path}` imports a source file inside the asset directory right away and returns its uuid. It runs on the editor thread, so a big model stalls the editor until it finishes.

Creating and assigning a material
1. `asset_create {type:"Material", path:"Assets/Materials/red"}` returns the uuid.
2. `asset_update {asset:"Assets/Materials/red", material:{albedo_color:{x:0.8,y:0.1,z:0.1}, roughness_factor:0.4}}`.
3. `asset_save {asset:"Assets/Materials/red"}` writes it. Edits stay in memory until then.
4. `entity_update` with `set:{MeshComponent:{material_uuid:"Assets/Materials/red"}}` assigns it.

Material fields (the same json `asset_get` shows and the sidecar stores, a patch changes only the keys you send)
- `albedo_color` x y z w, `emissive_color` x y z, `uv_size` and `uv_offset` x y. A vector patch may name only some axes.
- `roughness_factor`, `metallic_factor`, `normal_scale`, `occlusion_strength`, `alpha_cutoff`: numbers.
- `alpha_mode`: 0 Opaque, 1 Mask, 2 Blend. `sampling_mode`: 0 LinearRepeated, 1 LinearClamped, 2 NearestRepeated, 3 NearestClamped, 4 LinearRepeatedAnisotropy. `flip_normal_y`: boolean.
- `albedo_texture`, `normal_texture`, `emissive_texture`, `metallic_roughness_texture`, `occlusion_texture`: a texture asset's uuid or path, null clears. Texture reference counts follow the change.
- Example: `{"uuid":"...","sampling_mode":0,"albedo_color":{"x":0.8,"y":0.1,"z":0.1,"w":1},"roughness_factor":0.4,"alpha_mode":0,"albedo_texture":"..."}`

Particle systems and cinematics
- `asset_create` with type `ParticleSystem` or `Cinematic`. `asset_get` shows the whole document as json and `asset_update {asset, document:"<json>"}` replaces all of it. The document is parsed first, a bad one changes nothing.
- A particle or cinematic editor panel that has the asset open keeps a working copy and can overwrite your change.
- `asset_update` is not undoable, the editor's undo only covers scenes.

Loading and references
- `asset_load` loads an asset and takes one reference for MCP, each call takes another. `asset_load {unload:true}` releases one of those, never a reference the editor or a scene holds. MCP's references are released when the server stops. `asset_update` on an unloaded asset loads it the same way.
- `asset_list loaded_only` before and after shows what is held.

Thumbnails
- `asset_thumbnail {asset, size}` returns a PNG of a model, a material on a sphere or a texture. Other types have none. Models and materials render on a later frame of the editor, so it waits a moment.
