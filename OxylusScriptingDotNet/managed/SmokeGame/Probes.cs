using System;
using System.Numerics;

using Oxylus;

namespace SmokeGame;

// what the API tests read back: each probe copies what C# sees into a component the test checks natively

[Component]
public struct ApiProbe
{
    public float DeltaTime;
    public double Elapsed;
    public ulong Frame;
    public Vector3 WorldPosition;
    public Vector3 Forward;
    public int Runs;
}

[Component]
public struct PlaceProbe
{
    public Vector3 WorldPosition;
    public Vector3 LookAt;
    public int Apply;
}

[Component]
public struct InputProbe
{
    public int Bound;
    public int KeyHeld;
    public int KeyPressed;
    public int KeyReleased;
    public int MouseHeld;
    public int ActionPressed;
    public int ActionHeld;
    public int Gamepads;
    public int GamepadHeld;
    public float GamepadAxis;
    public Vector2 MousePosition;
}

public static class Probes
{
    static readonly InputAction Fire = new("probe_fire");

    [System(Phase.OnUpdate)]
    public static void ReadApi(Entity entity, ref ApiProbe probe)
    {
        probe.DeltaTime = Time.DeltaTime;
        probe.Elapsed = Time.Elapsed;
        probe.Frame = Time.Frame;
        if (entity.Has<TransformComponent>())
        {
            probe.WorldPosition = entity.WorldPosition;
            probe.Forward = entity.Get<TransformComponent>().Forward;
        }

        probe.Runs++;
        Log.Info($"api probe {probe.Runs} saw {probe.DeltaTime:F3}");
    }

    [System(Phase.OnUpdate)]
    public static void Place(Entity entity, ref PlaceProbe probe)
    {
        if (probe.Apply == 0)
        {
            return;
        }

        probe.Apply = 0;
        entity.WorldPosition = probe.WorldPosition;
        entity.LookAt(probe.LookAt, Vector3.UnitY);
    }

    [System(Phase.OnUpdate)]
    public static void ReadInput(ref InputProbe probe)
    {
        if (probe.Bound == 0)
        {
            Input.BindAction("probe_fire", InputBinding.Key(ScanCode.F), InputBinding.GamepadButton(GamepadButtonCode.South));
            probe.Bound = 1;
        }

        probe.KeyHeld = Input.IsKeyHeld(ScanCode.W) ? 1 : 0;
        probe.KeyPressed = Input.IsKeyPressed(ScanCode.W) ? 1 : 0;
        probe.KeyReleased = Input.IsKeyReleased(ScanCode.W) ? 1 : 0;
        probe.MouseHeld = Input.IsMouseHeld(MouseCode.Left) ? 1 : 0;
        probe.MousePosition = Input.MousePosition;
        probe.ActionPressed = Fire.IsPressed() ? 1 : 0;
        probe.ActionHeld = Fire.IsHeld() ? 1 : 0;

        Span<Gamepad> gamepads = stackalloc Gamepad[4];
        probe.Gamepads = Input.GetGamepads(gamepads);
        if (probe.Gamepads > 0)
        {
            probe.GamepadHeld = gamepads[0].IsHeld(GamepadButtonCode.East) ? 1 : 0;
            probe.GamepadAxis = gamepads[0].Axis(GamepadAxisCode.AxisLeftX);
        }
    }
}

// pushes its body sideways, and checks a static body turns a force away
public sealed class Pusher : Behaviour
{
    public override void OnStart()
    {
        Entity.Body.SetLinearVelocity(new Vector3(4.0f, 0.0f, 0.0f));
        try
        {
            World.Lookup("Floor").Body.AddForce(Vector3.UnitY);
        }
        catch (InvalidOperationException)
        {
            Log.Info("Pusher saw the floor refuse a force");
        }
    }

    public override void OnFixedUpdate(float deltaTime)
    {
        var body = Entity.Body;
        if (body.LinearVelocity.X < 1.0f)
        {
            body.AddImpulse(new Vector3(1.0f, 0.0f, 0.0f));
        }
    }
}

public sealed class RaycastProbe : SceneScript
{
    public override void OnStart()
    {
        var floor = World.Lookup("Floor");
        if (Physics.Raycast(World, new Vector3(0.0f, 5.0f, 0.0f), -Vector3.UnitY, out var hit))
        {
            Log.Info($"raycast hit {hit.Entity.Name} at {hit.Point.Y:F2} normal {hit.Normal.Y:F2} distance {hit.Distance:F2}");
        }

        var isMissed = !Physics.Raycast(World, new Vector3(0.0f, 5.0f, 0.0f), -Vector3.UnitY, out _, ignore: floor);
        var isShort = !Physics.Raycast(World, new Vector3(0.0f, 5.0f, 0.0f), -Vector3.UnitY, out _, maxDistance: 1.0f);
        var isFiltered = !Physics.Raycast(World, new Vector3(0.0f, 5.0f, 0.0f), -Vector3.UnitY, out _, layers: PhysicsLayers.Moving);
        Log.Info($"raycast misses: ignored {isMissed} short {isShort} filtered {isFiltered}");
    }
}

// walks along x and reports once it stands on something
public sealed class Walker : Behaviour
{
    bool isReported;

    public override void OnFixedUpdate(float deltaTime)
    {
        var character = Entity.Character;
        character.SetLinearVelocity(new Vector3(2.0f, character.LinearVelocity.Y, 0.0f));
        if (!isReported && character.IsGrounded)
        {
            isReported = true;
            Log.Info($"Walker grounded, normal {character.GroundNormal.Y:F1}");
        }
    }
}

// asset, audio, scene and VFS calls from a scene script, against the files TestDotNetScene writes into its assets
public sealed class ContentProbe : SceneScript
{
    AssetHandle? beep;

    public override void OnStart()
    {
        var id = Assets.Find("Audio/beep.wav");
        Log.Info($"content found {id.IsValid} missing {Assets.Find("Audio/nope.wav").IsValid}");

        beep = Assets.Acquire(id);
        Log.Info($"content acquired {beep.Type} loaded {beep.IsLoaded} refs {Assets.Info(id).RefCount}");

        var first = World.Spawn("Spawned");
        var second = World.Spawn("Spawned");
        var child = World.Spawn("Child", first);
        Log.Info($"content spawned {first != second} transform {first.Has<TransformComponent>()} parent {child.Parent == first}");

        var speaker = World.Lookup("Speaker").Audio;
        speaker.SetSound(id);
        speaker.Play();
        Log.Info($"content speaker playing {speaker.IsPlaying} length {speaker.Length:F2} refs {Assets.Info(id).RefCount}");

        Audio.PlayOneShot(beep, 0.5f);
        Audio.PlayOneShot(id, 1.0f, new Vector3(1.0f, 0.0f, 0.0f));

        Log.Info($"content vfs {Vfs.Exists(VfsDirectory.Assets, "Audio/beep.wav")} {Vfs.ReadAllText(VfsDirectory.Assets, "Config/hello.txt")}");
        Log.Info($"content virtual {Vfs.ToVirtual(Vfs.Resolve(VfsDirectory.Assets, "Audio/beep.wav"))}");

        try
        {
            World.SpawnModel(id);
        }
        catch (InvalidOperationException)
        {
            Log.Info("content refused a sound as a model");
        }

        World.Defer(() => Log.Info("content deferred ran"));
        Scene.Load("Scenes/next.oxscene");
    }

    public override void OnStop()
    {
        var id = beep!.Id;
        beep.Dispose();
        Log.Info($"content released, refs {Assets.Info(id).RefCount}");
    }
}

// a model's materials and one material's parameters, against the assets TestDotNetScene registers. It leaves the
// material pointing at textures a, b and c, and gives them up again when it stops
public sealed class MaterialProbe : SceneScript
{
    AssetId stone;
    AssetId a;
    AssetId b;
    AssetId c;

    public override void OnStart()
    {
        var model = Assets.Find("Models/crate.glb");
        stone = Assets.Find("Materials/stone.mat");
        var idle = Assets.Find("Materials/idle.mat");
        a = Assets.Find("Textures/a.png");
        b = Assets.Find("Textures/b.png");
        c = Assets.Find("Textures/c.png");

        var listed = Assets.GetMaterials(model);
        Log.Info($"material list {listed.Length} stone first {listed.Length > 0 && listed[0] == stone}");
        Span<AssetId> one = stackalloc AssetId[1];
        var total = Assets.GetMaterials(model, one);
        Log.Info($"material short list {total} stone first {one[0] == stone}");
        Log.Info($"material of a material {Assets.GetMaterials(stone).Length}");

        var isRead = Assets.GetMaterial(stone, out var data);
        Log.Info($"material read {isRead} color {data.AlbedoColor.X:F2} {data.AlbedoColor.Y:F2} {data.AlbedoColor.Z:F2} {data.AlbedoColor.W:F2} " +
                 $"uv {data.UvSize.X:F2} {data.UvSize.Y:F2} offset {data.UvOffset.X:F3} {data.UvOffset.Y:F2} " +
                 $"emissive {data.EmissiveColor.X:F1} {data.EmissiveColor.Y:F1} {data.EmissiveColor.Z:F1}");
        Log.Info($"material read roughness {data.RoughnessFactor:F3} metallic {data.MetallicFactor:F3} normal {data.NormalScale:F2} " +
                 $"occlusion {data.OcclusionStrength:F2} alpha {data.AlphaMode} cutoff {data.AlphaCutoff:F2} sampling {data.SamplingMode} flip {data.FlipNormalY}");
        Log.Info($"material read albedo a {data.AlbedoTexture == a} normal c {data.NormalTexture == c} emissive {data.EmissiveTexture.IsValid}");
        Log.Info($"material refs a {Assets.Info(a).RefCount} b {Assets.Info(b).RefCount} c {Assets.Info(c).RefCount}");

        data.AlbedoColor = new Vector4(0.5f, 0.25f, 0.125f, 1.0f);
        data.UvSize = new Vector2(3.0f, 5.0f);
        data.UvOffset = new Vector2(0.5f, 0.75f);
        data.EmissiveColor = new Vector3(4.0f, 5.0f, 6.0f);
        data.RoughnessFactor = 0.5f;
        data.MetallicFactor = 0.875f;
        data.NormalScale = 1.5f;
        data.OcclusionStrength = 0.25f;
        data.AlphaMode = AlphaMode.Blend;
        data.AlphaCutoff = 0.5f;
        data.SamplingMode = SamplingMode.LinearClamped;
        data.FlipNormalY = false;
        // a, b and c all change slots: b is new, a and c each move to another one
        data.AlbedoTexture = b;
        data.NormalTexture = a;
        data.EmissiveTexture = c;
        Assets.SetMaterial(stone, data);

        Assets.GetMaterial(stone, out var back);
        Log.Info($"material written color {back.AlbedoColor.X:F2} {back.AlbedoColor.Y:F2} {back.AlbedoColor.Z:F3} {back.AlbedoColor.W:F2} " +
                 $"uv {back.UvSize.X:F2} {back.UvSize.Y:F2} offset {back.UvOffset.X:F2} {back.UvOffset.Y:F2} " +
                 $"emissive {back.EmissiveColor.X:F1} {back.EmissiveColor.Y:F1} {back.EmissiveColor.Z:F1}");
        Log.Info($"material written roughness {back.RoughnessFactor:F3} metallic {back.MetallicFactor:F3} normal {back.NormalScale:F2} " +
                 $"occlusion {back.OcclusionStrength:F2} alpha {back.AlphaMode} cutoff {back.AlphaCutoff:F2} sampling {back.SamplingMode} flip {back.FlipNormalY}");
        Log.Info($"material written albedo b {back.AlbedoTexture == b} normal a {back.NormalTexture == a} emissive c {back.EmissiveTexture == c}");
        Log.Info($"material refs a {Assets.Info(a).RefCount} b {Assets.Info(b).RefCount} c {Assets.Info(c).RefCount}");

        var wrongSlot = data;
        wrongSlot.NormalTexture = stone;
        var wrongMode = data;
        wrongMode.AlphaMode = (AlphaMode)9;
        Log.Info($"material refused idle {Throws(() => Assets.SetMaterial(idle, data))} texture {Throws(() => Assets.SetMaterial(a, data))} " +
                 $"unknown {Throws(() => Assets.SetMaterial(default, data))} slot {Throws(() => Assets.SetMaterial(stone, wrongSlot))} " +
                 $"mode {Throws(() => Assets.SetMaterial(stone, wrongMode))}");
        Log.Info($"material unread idle {Assets.GetMaterial(idle, out _)} texture {Assets.GetMaterial(a, out _)} unknown {Assets.GetMaterial(default, out _)}");
        Log.Info($"material refs after refusals a {Assets.Info(a).RefCount} b {Assets.Info(b).RefCount} c {Assets.Info(c).RefCount}");
    }

    public override void OnStop()
    {
        Assets.GetMaterial(stone, out var data);
        data.AlbedoTexture = default;
        data.NormalTexture = default;
        data.EmissiveTexture = default;
        Assets.SetMaterial(stone, data);
        Log.Info($"material cleared refs a {Assets.Info(a).RefCount} b {Assets.Info(b).RefCount} c {Assets.Info(c).RefCount}");
    }

    static bool Throws(Action action)
    {
        try
        {
            action();
            return false;
        }
        catch (InvalidOperationException)
        {
            return true;
        }
    }
}

// keeps a handle it never disposes, the runtime gives the reference back when the scripts unload
public sealed class LeakyProbe : SceneScript
{
    public AssetHandle? Held { get; private set; }

    public override void OnStart() => Held = Assets.Acquire("Audio/beep.wav");
}

[Component]
public struct UiProbe
{
    public int Phase;
    public int Score;
    public int ListenerHits;
}

// what Aabb and Mathx are given and what they give back, TestDotNetApi compares it with glm and ox::AABB. The matrix
// arrives as glm's columns
[Component]
public struct MathProbe
{
    public int Run;
    public Vector3 BoxMin;
    public Vector3 BoxMax;
    public Vector3 OtherMin;
    public Vector3 OtherMax;
    public Vector3 Point;
    public Vector3 Scale;
    public Vector3 Direction;
    public Vector3 Up;
    public Vector4 Column0;
    public Vector4 Column1;
    public Vector4 Column2;
    public Vector4 Column3;

    public Vector3 TransformedMin;
    public Vector3 TransformedMax;
    public Vector3 TranslatedMin;
    public Vector3 TranslatedMax;
    public Vector3 ScaledMin;
    public Vector3 ScaledMax;
    public Vector3 MergedMin;
    public Vector3 MergedMax;
    public Vector3 Center;
    public Vector3 Extents;
    public Vector3 Size;
    public Vector3 Floored;
    public Vector3 Ceiled;
    public Vector3 Rounded;
    public int Intersects;
    public int Classification;
    public int Contains;
    public Quaternion Look;
}

public static class MathProbes
{
    [System(Phase.OnUpdate)]
    public static void Compute(ref MathProbe probe)
    {
        if (probe.Run == 0)
        {
            return;
        }

        probe.Run = 0;
        var box = new Aabb(probe.BoxMin, probe.BoxMax);
        var other = new Aabb(probe.OtherMin, probe.OtherMax);
        // glm's columns are System.Numerics' rows
        var matrix = new Matrix4x4(
            probe.Column0.X, probe.Column0.Y, probe.Column0.Z, probe.Column0.W,
            probe.Column1.X, probe.Column1.Y, probe.Column1.Z, probe.Column1.W,
            probe.Column2.X, probe.Column2.Y, probe.Column2.Z, probe.Column2.W,
            probe.Column3.X, probe.Column3.Y, probe.Column3.Z, probe.Column3.W);

        var transformed = box.Transform(matrix);
        probe.TransformedMin = transformed.Min;
        probe.TransformedMax = transformed.Max;
        var translated = box.Translate(probe.Point);
        probe.TranslatedMin = translated.Min;
        probe.TranslatedMax = translated.Max;
        var scaled = box.Scale(probe.Scale);
        probe.ScaledMin = scaled.Min;
        probe.ScaledMax = scaled.Max;
        var merged = box.Merge(other);
        probe.MergedMin = merged.Min;
        probe.MergedMax = merged.Max;
        probe.Center = box.Center;
        probe.Extents = box.Extents;
        probe.Size = box.Size;
        probe.Intersects = box.Intersects(other) ? 1 : 0;
        probe.Classification = (int)box.Classify(other);
        probe.Contains = box.Contains(probe.Point) ? 1 : 0;

        probe.Look = Mathx.QuatLookAt(probe.Direction, probe.Up);
        probe.Floored = Mathx.Floor(probe.Point);
        probe.Ceiled = Mathx.Ceil(probe.Point);
        probe.Rounded = Mathx.Round(probe.Point);
    }
}

[Component]
public struct NetProbe
{
    public int Phase;
    public int Port;
}

// drives a document and a data model in whatever RmlUi context the world has, TestDotNetUi gives it one
public static class UiProbes
{
    static UiDataModel? model;
    static int score;
    static int listenerHits;

    [System(Phase.OnUpdate)]
    public static void DriveUi(Entity entity, ref UiProbe probe)
    {
        if (probe.Phase == 0)
        {
            probe.Phase = 1;
            score = 0;
            listenerHits = 0;

            var world = entity.World;
            model = Ui.CreateDataModel(world, "probe");
            model.Bind("score", () => score, value => score = value);
            model.Bind("label", () => $"score is {score}");
            model.BindEvent("add", (e, args) =>
            {
                score += (int)args.GetInteger(0);
                model.Dirty("score");
                model.Dirty("label");
                Log.Info($"ui add {args.GetInteger(0)} from {e.Type}");
            });

            var document = Ui.LoadDocument(world, "UI/probe.rml");
            document.Show();
            document.GetElementById("title").SetInnerRml("Hello");
            document.GetElementById("title").SetClass("lit", true);
            document.GetElementById("button").AddEventListener("click", e =>
            {
                listenerHits++;
                Log.Info($"ui listener {e.Type} on {e.Current.GetAttribute("id")}");
            });
        }

        probe.Score = score;
        probe.ListenerHits = listenerHits;
    }
}

// a server and a client in one process over localhost, RPCs both ways, TestDotNetNet steps it
public static class NetProbes
{
    static readonly Rpc<int, string> Hello = new("probe_hello");
    static readonly Rpc<float, byte[], long> Reply = new("probe_reply");
    static readonly Rpc<float, float, float, float> Quad = new("probe_quad");
    static NetServer? server;
    static NetClient? client;

    static string Sorted(ReadOnlySpan<ulong> ids)
    {
        var sorted = ids.ToArray();
        Array.Sort(sorted);
        return string.Join(",", sorted);
    }

    [System(Phase.OnUpdate)]
    public static void DriveNet(ref NetProbe probe)
    {
        if (probe.Phase == 0)
        {
            probe.Phase = 1;
            server = NetServer.Start((ushort)probe.Port);
            server.ClientConnected += id => Log.Info("server saw a client connect");
            server.ClientDisconnected += id => Log.Info("server saw a client disconnect");
            server.ClientAcked += (id, sequence) => Log.Info($"server got ack {sequence} from {id.Value}");
            Hello.On(server, (from, number, text) =>
            {
                Log.Info($"server got {number} {text}");
                Reply.Send(server, from, 1.5f, [1, 2, 3], 7L);
            });
            Quad.On(server, (from, a, b, c, d) =>
            {
                Log.Info($"server got quad {a} {b} {c} {d}");
                Quad.Send(server, from, d, c, b, a);
            });

            client = NetClient.Connect("127.0.0.1", (ushort)probe.Port, 2000);
            var connecting = client;
            connecting.Connected += () =>
            {
                Log.Info($"client connected {connecting.Status}");
                Hello.Send(connecting, 42, "hi");
                Quad.Send(connecting, 1.0f, 2.0f, 3.0f, 4.0f);
            };
            Quad.On(client, (a, b, c, d) => Log.Info($"client got quad {a} {b} {c} {d}"));
            Reply.On(client, (number, bytes, count) => Log.Info($"client got {number:F1} {bytes.Length} bytes {count}"));
            client.SceneSnapshot += (sequence, changed, removed) =>
                Log.Info($"client got snapshot {sequence} changed {Sorted(changed)} removed {Sorted(removed)}");
        }
        else if (probe.Phase == 2)
        {
            probe.Phase = 3;
            client?.Dispose();
            client = null;
        }
        else if (probe.Phase == 4)
        {
            probe.Phase = 5;
            server?.Dispose();
            server = null;
        }
    }
}

[Component]
public struct Marker;

[Component]
public struct Likes;

[Component]
public struct Apples;

// a struct needs an explicit constructor to have field initializers, which is what Add<T>() has to honour
[Component]
public struct Tuned
{
    public float Speed = 2.5f;
    public int Level = 7;
    public Vector3 Scale = Vector3.One;

    public Tuned()
    {
    }
}

[Component]
public struct Calm
{
    public float Speed;
    public int Level;
}

// the ECS API runs outside of a frame in a behaviour's OnStart, where a system's commands would be deferred already,
// and reports what it saw in the log
public sealed class PathProbe : Behaviour
{
    public override void OnStart()
    {
        var parent = World.CreateEntity("PathParent");
        var first = World.CreateEntity("PathA").ChildOf(parent);
        var second = World.CreateEntity("PathB").ChildOf(parent);
        var nested = World.CreateEntity("PathC").ChildOf(first);
        Log.Info($"ecs path {nested.Path}");

        var count = 0;
        var isMatching = true;
        foreach (var child in parent.Children)
        {
            count++;
            isMatching &= child == first || child == second;
        }

        var hasLeafChildren = false;
        foreach (var _ in nested.Children)
        {
            hasLeafChildren = true;
            break;
        }

        Log.Info($"ecs children {count} matching {isMatching} leaf {hasLeafChildren}");

        var lengthy = World.CreateEntity(new string('x', 300));
        Log.Info($"ecs long path {lengthy.Path.Length}");
    }
}

public sealed class EnsureProbe : Behaviour
{
    public override void OnStart()
    {
        var entity = World.CreateEntity("EnsureTarget");
        ref var velocity = ref entity.Ensure<Velocity>();
        velocity.Value = new Vector3(1.0f, 2.0f, 3.0f);
        ref var again = ref entity.Ensure<Velocity>();
        Log.Info($"ecs ensure {again.Value.X} {again.Value.Z} has {entity.Has<Velocity>()}");
    }
}

public sealed class PairProbe : Behaviour
{
    public override void OnStart()
    {
        var subject = World.CreateEntity("PairSubject");
        var target = World.CreateEntity("PairTarget");
        var relation = World.CreateEntity("PairRelation");

        Log.Info($"ecs pair before {subject.HasPair<Likes>(target)} {subject.HasPair(relation, target)}");

        subject.AddPair<Likes>(target).AddPair(relation, target);
        Log.Info($"ecs pair added {subject.HasPair<Likes>(target)} {subject.HasPair(relation, target)}");
        Log.Info($"ecs pair targets {subject.Target<Likes>() == target} {subject.Target(relation) == target} {subject.Target<Likes>(1).Id}");

        subject.AddPair<Likes, Apples>();
        Log.Info($"ecs pair types {subject.HasPair<Likes, Apples>()}");

        subject.RemovePair<Likes>(target).RemovePair(relation, target).RemovePair<Likes, Apples>();
        Log.Info($"ecs pair removed {subject.HasPair<Likes>(target)} {subject.HasPair(relation, target)} {subject.HasPair<Likes, Apples>()}");
    }
}

public sealed class IsAProbe : Behaviour
{
    public override void OnStart()
    {
        var prefab = World.CreateEntity("IsAPrefab");
        prefab.Set(new Velocity { Value = new Vector3(4.0f, 0.0f, 0.0f) });
        var instance = World.CreateEntity("IsAInstance").IsA(prefab);
        Log.Info($"ecs isa {instance.Has<Velocity>()} {instance.Get<Velocity>().Value.X}");
    }
}

public sealed class DeferProbe : Behaviour
{
    public override void OnStart()
    {
        var entity = World.CreateEntity("DeferTarget");
        World.DeferBegin();
        entity.Add<Marker>();
        var isHeld = !entity.Has<Marker>();
        World.DeferEnd();
        Log.Info($"ecs defer held {isHeld} after {entity.Has<Marker>()}");

        var immediate = World.CreateEntity("DeferSuspended");
        World.DeferBegin();
        World.DeferSuspend();
        immediate.Add<Marker>();
        var isApplied = immediate.Has<Marker>();
        World.DeferResume();
        World.DeferEnd();
        Log.Info($"ecs defer suspended {isApplied}");
    }
}

public sealed class DefaultsProbe : Behaviour
{
    public override void OnStart()
    {
        var added = World.CreateEntity("DefaultsAdded").Add<Tuned>();
        var tuned = added.Get<Tuned>();
        Log.Info($"ecs defaults added {tuned.Speed} {tuned.Level} {tuned.Scale.Z}");

        var ensured = World.CreateEntity("DefaultsEnsured");
        Log.Info($"ecs defaults ensured {ensured.Ensure<Tuned>().Level}");

        var calm = World.CreateEntity("DefaultsCalm").Add<Calm>().Get<Calm>();
        Log.Info($"ecs defaults calm {calm.Speed} {calm.Level}");
    }
}

// the scene operations of a world, which a bare world answers with nothing
public sealed class SceneInfoProbe : Behaviour
{
    public override void OnStart()
    {
        Log.Info($"scene uuid {World.Uuid} valid {World.Uuid.IsValid}");
        World.SetUiDpiRatio(2.0f);
        World.ClearUiDpiRatio();
        Log.Info("scene dpi set and cleared");
    }

    public override void OnUpdate(float deltaTime) => Log.Info($"scene focused {World.InputFocused}");
}

public sealed class ParticleProbe : Behaviour
{
    public override void OnStart()
    {
        var emitter = World.Lookup("Emitter").Particles;
        Log.Info($"particles awake {emitter.IsPlaying}");

        emitter.Play();
        var isPlaying = emitter.IsPlaying;
        emitter.EmitBurst(5);
        emitter.EmitBurst(3);
        emitter.SetParameter(2, new Vector4(1.0f, 2.0f, 3.0f, 4.0f));
        var isNamed = emitter.SetParameter("missing", Vector4.One);
        emitter.Stop();
        var isStopped = !emitter.IsPlaying;
        Log.Info($"particles played {isPlaying} stopped {isStopped} named {isNamed}");

        emitter.Restart();
        Log.Info($"particles restarted {emitter.IsPlaying}");

        var plain = World.Lookup("Plain").Particles;
        plain.Play();
        plain.EmitBurst(1);
        plain.SetParameter(0, Vector4.One);
        Log.Info($"particles plain {plain.IsPlaying}");
    }
}

public sealed class CinematicProbe : Behaviour
{
    public override void OnStart()
    {
        var running = World.Lookup("PlayerA").Cinematic;
        Log.Info($"cinematic idle {running.Time} {running.Duration}");
        running.Play();
        running.Seek(5.0f);
        Log.Info($"cinematic seeked {running.Time} {running.Duration}");

        var stopped = World.Lookup("PlayerB").Cinematic;
        stopped.Play();
        stopped.Stop();

        var plain = World.Lookup("Plain").Cinematic;
        plain.Play();
        plain.Seek(1.0f);
        Log.Info($"cinematic plain {plain.Time} {plain.Duration}");
    }
}

public sealed class UiCacheProbe : Behaviour
{
    public override void OnStart()
    {
        Ui.ClearStyleSheetCache();
        Ui.ClearTemplateCache();
        Log.Info("ui caches cleared");
    }
}

public sealed class AppProbe : Behaviour
{
    public override void OnStart()
    {
        Window.Resized += (width, height) => Log.Info($"window resized {width} {height}");
        Log.Info($"window size {Window.Size.Width} {Window.Size.Height} logical {Window.LogicalSize.Width} {Window.LogicalSize.Height}");

        Log.Info($"time default {Time.MaxFrameTime}");
        Time.MaxFrameTime = 8.0;
        Log.Info($"time limited {Time.MaxFrameTime}");
    }
}

public sealed class QuitProbe : Behaviour
{
    public override void OnStart()
    {
        App.Quit();
        Log.Info("app quit called");
    }
}

public sealed class SpriteProbe : Behaviour
{
    public override void OnStart()
    {
        var first = World.CreateEntity("SpriteA").Add<SpriteComponent>();
        var second = World.CreateEntity("SpriteB").Add<SpriteComponent>();
        var a = first.Get<SpriteComponent>().Material;
        var b = second.Get<SpriteComponent>().Material;
        Log.Info($"sprite materials {a} {b} valid {a.IsValid && b.IsValid}");

        first.Get<SpriteComponent>().Material = b;
        second.Get<SpriteComponent>().Material = a;
        Log.Info("sprite swapped");
    }
}

public sealed class AppResetProbe : Behaviour
{
    public override void OnStart()
    {
        Time.ResetMaxFrameTime();
        Log.Info($"time reset {Time.MaxFrameTime}");
    }
}

// each BodyProbe command runs once on the next update, then the body's getters are copied back for the test to check
[Component]
public struct BodyProbe
{
    public int Command;
    public float Scalar;
    public Vector3 Vector;
    public Vector3 Point;
    public Quaternion Rotation;

    public int Threw;
    public int MotionType;
    public int ObjectLayer;
    public float Friction;
    public float Restitution;
    public int Flags;
    public Vector3 CenterOfMass;
    public Vector3 BoundsMin;
    public Vector3 BoundsMax;
    public Vector3 Force;
    public Vector3 Torque;
    public Vector3 PointVelocity;
    public Vector3 Position;
    public int IsActive;
    public int Kind;
}

[Component]
public struct VehicleProbe
{
    public int Exists;
    public int Gear;
    public float EngineRpm;
    public float EngineAngularVelocity;
    public float ForwardSpeed;
    public int Wheels;
    public int Contacting;
    public float MaxSlip;
    public int Threw;
}

[Component]
public struct CameraProbe
{
    public Vector2 ScreenPosition;
    public Vector2 ScreenSize;
    public Vector3 Origin;
    public Vector3 Direction;
    public Vector3 Point;
    public int Threw;
    public int HitIsWall;
    public float HitDistance;
}

public static class PhysicsProbes
{
    [System(Phase.OnUpdate)]
    public static void DriveBody(Entity entity, ref BodyProbe probe)
    {
        var command = probe.Command;
        if (command == 0)
        {
            return;
        }

        probe.Command = 0;
        probe.Threw = 0;
        var body = entity.Body;
        try
        {
            switch (command)
            {
                case 1:
                    body.SetMotionType((MotionType)(int)probe.Scalar);
                    break;
                case 2:
                    body.SetObjectLayer((PhysicsLayers)(uint)probe.Scalar);
                    break;
                case 3:
                    body.SetFriction(probe.Scalar);
                    body.SetRestitution(probe.Vector.X);
                    break;
                case 4:
                    var bits = (int)probe.Scalar;
                    body.SetIsSensor((bits & 1) != 0);
                    body.SetCollideKinematicVsNonDynamic((bits & 2) != 0);
                    body.SetUseManifoldReduction((bits & 4) != 0);
                    body.SetApplyGyroscopicForce((bits & 8) != 0);
                    body.SetEnhancedInternalEdgeRemoval((bits & 16) != 0);
                    break;
                case 5:
                    body.AddForce(probe.Vector, probe.Point);
                    break;
                case 6:
                    body.AddImpulse(probe.Vector, probe.Point);
                    break;
                case 7:
                    body.MoveKinematic(probe.Point, probe.Rotation, probe.Scalar);
                    break;
                case 8:
                    body.SetPositionAndRotation(probe.Point, probe.Rotation);
                    break;
                case 9:
                    body.AddLinearVelocity(probe.Vector);
                    break;
                case 10:
                    body.SetPosition(probe.Point);
                    break;
                case 11:
                    body.SetRotation(probe.Rotation);
                    break;
                case 12:
                    entity.Get<RigidBodyComponent>().AllowedDofs = (AllowedDofs)(uint)probe.Scalar;
                    break;
                case 13:
                    body.AddTorque(probe.Vector);
                    break;
            }
        }
        catch (InvalidOperationException)
        {
            probe.Threw = 1;
        }

        if (!body.Exists)
        {
            return;
        }

        probe.MotionType = (int)body.MotionType;
        probe.Kind = (body.IsStatic ? 1 : 0) | (body.IsKinematic ? 2 : 0) | (body.IsDynamic ? 4 : 0);
        probe.ObjectLayer = (int)body.ObjectLayer;
        probe.Friction = body.Friction;
        probe.Restitution = body.Restitution;
        probe.Flags = (body.IsSensor ? 1 : 0)
            | (body.CollideKinematicVsNonDynamic ? 2 : 0)
            | (body.UseManifoldReduction ? 4 : 0)
            | (body.ApplyGyroscopicForce ? 8 : 0)
            | (body.EnhancedInternalEdgeRemoval ? 16 : 0);
        probe.CenterOfMass = body.CenterOfMassPosition;
        var bounds = body.WorldBounds;
        probe.BoundsMin = bounds.Min;
        probe.BoundsMax = bounds.Max;
        probe.Force = body.AccumulatedForce;
        probe.Torque = body.AccumulatedTorque;
        probe.PointVelocity = body.GetPointVelocity(probe.Point);
        probe.Position = body.Position;
        probe.IsActive = body.IsActive ? 1 : 0;
    }

    [System(Phase.OnUpdate)]
    public static void ReadVehicle(Entity entity, ref VehicleProbe probe)
    {
        var vehicle = entity.Vehicle;
        probe.Exists = vehicle.Exists ? 1 : 0;
        probe.Wheels = 0;
        probe.Contacting = 0;
        probe.MaxSlip = 0.0f;
        probe.Threw = 0;
        try
        {
            probe.Gear = vehicle.Gear;
            probe.EngineRpm = vehicle.EngineRpm;
            probe.EngineAngularVelocity = vehicle.EngineAngularVelocity;
            probe.ForwardSpeed = vehicle.ForwardSpeed;
            foreach (var child in entity.Children)
            {
                if (!child.Has<VehicleWheelComponent>())
                {
                    continue;
                }

                var wheel = child.VehicleWheel;
                probe.Wheels++;
                probe.Contacting += wheel.IsContacting ? 1 : 0;
                probe.MaxSlip = MathF.Max(probe.MaxSlip, MathF.Max(MathF.Abs(wheel.LongitudinalSlip), MathF.Abs(wheel.LateralSlip)));
            }
        }
        catch (InvalidOperationException)
        {
            probe.Threw = 1;
        }
    }

    [System(Phase.OnUpdate)]
    public static void ReadCamera(Entity entity, ref CameraProbe probe)
    {
        probe.Threw = 0;
        try
        {
            var ray = Camera.ScreenRay(entity, probe.ScreenPosition, probe.ScreenSize);
            probe.Origin = ray.Origin;
            probe.Direction = ray.Direction;
            probe.Point = ray.GetPoint(2.0f);
            if (Physics.Raycast(entity.World, ray, out var hit))
            {
                probe.HitIsWall = hit.Entity.Name == "Wall" ? 1 : 0;
                probe.HitDistance = hit.Distance;
            }
        }
        catch (InvalidOperationException)
        {
            probe.Threw = 1;
        }
    }
}
