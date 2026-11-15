# C# scripting

Project
- A project with C# scripting has a scripts project, `Scripts/<Name>.csproj` by default. `script_status` shows its path and state, `project_get` shows `scripts project`. If it says `closed`, the project has none or this editor has no C# support.
- New scripts are `.cs` files in the project's asset directory, so they sit next to the content.

Building
- The editor watches the scripts project and the asset directory. Saving a `.cs`, `.csproj` or `.props` file rebuilds in the background after a short pause, and a good build loads right away.
- `script_build` requests a build now. With `wait` (default) it returns when the build is done and loaded.
- A failed build keeps the last good build loaded. The result lists `errors N, warnings N` and the first 20 diagnostics as `severity file:line:col code message`, files project-relative. `log_read` has the rest.
- `script_status` reads the same block without building. `editor_wait` with `scripts` waits for background builds.
- `dotnet missing` means the .NET SDK could not be started, which is not a fault of the scripts.

Writing scripts
- `script_create` takes `path` (a directory inside the assets), `name` and `kind`: `EntityBehaviour` or `SceneBehaviour`. It writes `<ClassName>.cs` from the editor template and returns the path.
- Edit the file with your own file tools, then `script_build`.
- `script_types` lists the types of the loaded build as `name kind`. It needs a successful build first.

Entity behaviours
- Put one on an entity with `entity_update` and `set`: `{"entity":"Player","set":{"ScriptComponent":{"type_name":"MyGame.PlayerMotor"}}}` using a name from `script_types`.
- Scripts run only while the scene plays.

Scene behaviours
- `scene_scripts` without `add` or `remove` lists the scene's scripts. `add` takes SceneBehaviour type names, `remove` takes names the scene has. Scene scripts are saved with the scene.
- One call is one undo step labelled `mcp: scene_scripts`. Undoing a remove creates a fresh script, the removed script's runtime state is not restored.

Play mode
- `scene_play` waits for a pending build and refuses to start when the scripts do not compile.
- While a scene plays, a finished build is not loaded. It loads after `scene_stop`. `script_build` then returns after the build with a note saying so.
- Edits to a play scene, including `scene_scripts`, are not undoable and vanish at Stop.
