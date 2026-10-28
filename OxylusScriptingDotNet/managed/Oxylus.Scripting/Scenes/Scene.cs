using System;
using System.Text;

using Oxylus.Hosting;
using Oxylus.Interop;

namespace Oxylus;

// a scene's own operations on its world. Only a scene's world has them, a bare world's spawns return default
public static unsafe class Scene
{
    extension(World world)
    {
        // a scene entity with the TransformComponent and LayerComponent every one has, under `parent` when given. The
        // name is made unique, so spawning "Bullet" twice gives two entities
        public Entity Spawn(string name = "", Entity parent = default)
        {
            var bytes = Encoding.UTF8.GetBytes(name);
            fixed (byte* data = bytes)
            {
                return Checked(world, Runtime.Native.SceneSpawn(world.Handle, new StringView(data, bytes.Length), parent.Id), "an entity");
            }
        }

        // the model's node hierarchy with its meshes, loaded now if it wasn't
        public Entity SpawnModel(AssetId model, Entity parent = default) =>
            Checked(world, Runtime.Native.SceneSpawnModel(world.Handle, &model, parent.Id), $"model {model}");

        public Entity SpawnParticles(AssetId particleSystem) =>
            Checked(world, Runtime.Native.SceneSpawnParticles(world.Handle, &particleSystem), $"particle system {particleSystem}");

        // whether the scene takes input, false while the editor's focus is elsewhere, and for a world without a scene
        public bool InputFocused => Runtime.Native.SceneInputFocused(world.Handle);

        // the scene asset's id, invalid for a world without a scene
        public AssetId Uuid
        {
            get
            {
                AssetId id;
                Runtime.Native.SceneUuid(world.Handle, &id);
                return id;
            }
        }

        // writes the world as the scene's editor JSON to a physical path
        public void Save(string path)
        {
            var bytes = Encoding.UTF8.GetBytes(path);
            fixed (byte* data = bytes)
            {
                var status = Runtime.Native.SceneSave(world.Handle, new StringView(data, bytes.Length));
                if (status != OxStatus.Ok)
                {
                    throw new InvalidOperationException($"saving the scene to '{path}' failed: {status}");
                }
            }
        }

        // pins the scene's UI to `ratio` pixels per logical pixel instead of following the window
        public void SetUiDpiRatio(float ratio) => Runtime.Native.SceneSetUiDpiRatio(world.Handle, ratio);

        public void ClearUiDpiRatio() => Runtime.Native.SceneClearUiDpiRatio(world.Handle);

        // presses a flat track into the scene's terrain along the waypoints. Widths are the flat part in metres, shoulders
        // the blend back into the terrain on each side, and smoothing the metres along the track its height is averaged
        // over. No waypoints clears it
        public void SetTerrainTrack(ReadOnlySpan<TerrainWaypoint> waypoints, float smoothing)
        {
            const int MaxWaypoints = 1024;
            const string Accepted = "A terrain track takes 0 or 2 to 1024 waypoints, positive finite widths, and non-negative finite shoulders and smoothing";
            // checked before the stackalloc, which is what keeps it small
            if (waypoints.Length > MaxWaypoints)
            {
                throw new ArgumentException(Accepted);
            }

            Span<TerrainWaypointNative> native = stackalloc TerrainWaypointNative[waypoints.Length];
            for (var i = 0; i < waypoints.Length; i++)
            {
                native[i] = new TerrainWaypointNative { Position = waypoints[i].Position, Width = waypoints[i].Width, Shoulder = waypoints[i].Shoulder };
            }

            OxStatus status;
            fixed (TerrainWaypointNative* data = native)
            {
                status = Runtime.Native.SceneSetTerrainTrack(world.Handle, data, native.Length, smoothing);
            }

            if (status == OxStatus.NotFound)
            {
                throw new InvalidOperationException("Only a scene's world has a terrain track");
            }

            if (status != OxStatus.Ok)
            {
                throw new ArgumentException(Accepted);
            }
        }

        public void ClearTerrainTrack() => world.SetTerrainTrack(default, 0.0f);

        // runs `action` at the start of the scene's next update, after this frame's hooks and systems are done with
        // whatever it changes
        public void Defer(Action action)
        {
            ArgumentNullException.ThrowIfNull(action);
            Worlds.Get(world.Handle).Deferred.Add(action);
        }
    }

    // asks the scene's owner, a game or the editor, to replace the running scene with `scene`. It happens after the
    // current frame, never inside it
    public static void Load(AssetId scene)
    {
        if (Runtime.Native.SceneRequestLoad(&scene) != OxStatus.Ok)
        {
            throw new InvalidOperationException($"{scene} isn't a scene asset");
        }
    }

    public static void Load(string path)
    {
        var scene = Assets.Find(path);
        if (!scene.IsValid)
        {
            throw new InvalidOperationException($"No scene was imported from {path}");
        }

        Load(scene);
    }

    static Entity Checked(World world, ulong entity, string what)
    {
        if (entity == 0)
        {
            throw new InvalidOperationException($"Couldn't spawn {what}: only a scene's world spawns, and a model or particle system has to be an asset of that type");
        }

        return new Entity(world.Handle, entity);
    }
}
