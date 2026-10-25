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
